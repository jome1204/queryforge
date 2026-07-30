#!/usr/bin/env python3
"""Compare, validate, and summarize QueryForge database images.

This tool deliberately implements its own bounded reader instead of importing the
main utility module.  It is useful when investigating whether a writer changed
the on-disk representation without changing logical table contents.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

DATABASE_SIGNATURE = b"QFENG1\x00\x00"
RECORD_SIGNATURE = b"QFR1"
MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_TABLES = 4096
MAX_COLUMNS = 1024
MAX_ROWS = 1_000_000
MAX_VALUE_BYTES = 4 * 1024 * 1024


class FormatError(ValueError):
    """Raised for a malformed or unsupported database image."""


@dataclasses.dataclass(frozen=True)
class Column:
    name: str
    type_code: int
    nullable: bool
    primary_key: bool
    unique: bool


@dataclasses.dataclass(frozen=True)
class Table:
    name: str
    columns: tuple[Column, ...]
    rows: tuple[tuple[Any, ...], ...]


@dataclasses.dataclass(frozen=True)
class DatabaseImage:
    version: int
    generation: int
    flags: int
    tables: tuple[Table, ...]
    digest: str


class Reader:
    """Bounds-checked little-endian reader."""

    def __init__(self, data: bytes):
        self.data = data
        self.position = 0

    @property
    def remaining(self) -> int:
        return len(self.data) - self.position

    def take(self, count: int) -> bytes:
        if count < 0 or count > self.remaining:
            raise FormatError(
                f"need {count} bytes at offset {self.position}, "
                f"only {self.remaining} remain"
            )
        result = self.data[self.position : self.position + count]
        self.position += count
        return result

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def text16(self) -> str:
        size = self.u16()
        if size > 65535:
            raise FormatError("text length exceeds representation")
        return self.take(size).decode("utf-8", errors="strict")

    def text32(self) -> str:
        size = self.u32()
        if size > MAX_VALUE_BYTES:
            raise FormatError("text length exceeds resource limit")
        return self.take(size).decode("utf-8", errors="strict")


def _decode_value(reader: Reader) -> Any:
    type_code = reader.u8()
    if type_code == 0:
        return None
    if type_code == 1:
        return struct.unpack("<q", reader.take(8))[0]
    if type_code == 2:
        value = struct.unpack("<d", reader.take(8))[0]
        if value != value:
            return {"float": "nan"}
        if value == float("inf"):
            return {"float": "+inf"}
        if value == float("-inf"):
            return {"float": "-inf"}
        return value
    if type_code == 3:
        length = reader.u32()
        if length > MAX_VALUE_BYTES:
            raise FormatError("text value exceeds resource limit")
        return reader.take(length).decode("utf-8", errors="replace")
    if type_code == 4:
        raw = reader.u8()
        if raw not in (0, 1):
            raise FormatError("invalid Boolean representation")
        return bool(raw)
    if type_code == 5:
        length = reader.u32()
        if length > MAX_VALUE_BYTES:
            raise FormatError("blob value exceeds resource limit")
        return {"blob": reader.take(length).hex()}
    raise FormatError(f"unknown value type {type_code}")


def _decode_record(payload: bytes, expected_columns: int) -> tuple[Any, ...]:
    if len(payload) < 27 or payload[:4] != RECORD_SIGNATURE:
        raise FormatError("invalid record signature")
    stored_crc = struct.unpack_from("<I", payload, len(payload) - 4)[0]
    actual_crc = zlib.crc32(payload[:-4]) & 0xFFFFFFFF
    if stored_crc != actual_crc:
        raise FormatError(
            f"record checksum mismatch: {stored_crc:08x} != {actual_crc:08x}"
        )
    reader = Reader(payload[4:-4])
    reader.u64()
    reader.u64()
    if reader.u8() > 1:
        raise FormatError("record deletion flag is invalid")
    count = reader.u16()
    if count != expected_columns:
        raise FormatError(
            f"record has {count} fields, table schema has {expected_columns}"
        )
    values = tuple(_decode_value(reader) for _ in range(count))
    if reader.remaining:
        raise FormatError(f"record has {reader.remaining} trailing bytes")
    return values


def parse_database(path: Path) -> DatabaseImage:
    data = path.read_bytes()
    if len(data) > MAX_FILE_BYTES:
        raise FormatError(f"{path}: database exceeds {MAX_FILE_BYTES} bytes")
    if len(data) < 32:
        raise FormatError(f"{path}: truncated database header")
    stored_crc = struct.unpack_from("<I", data, 28)[0]
    actual_crc = zlib.crc32(data[32:]) & 0xFFFFFFFF
    if stored_crc != actual_crc:
        raise FormatError(
            f"{path}: database checksum mismatch "
            f"{stored_crc:08x} != {actual_crc:08x}"
        )
    reader = Reader(data)
    if reader.take(8) != DATABASE_SIGNATURE:
        raise FormatError(f"{path}: invalid database signature")
    version = reader.u32()
    table_count = reader.u32()
    declared_rows = reader.u64()
    page_size = reader.u32()
    reader.u32()
    if version != 1:
        raise FormatError(f"{path}: unsupported database version {version}")
    if page_size != 4096:
        raise FormatError(f"{path}: unsupported page size {page_size}")
    if table_count > MAX_TABLES:
        raise FormatError(f"{path}: table count exceeds resource limit")

    tables: list[Table] = []
    names: set[str] = set()
    total_rows = 0
    generation = 0
    for table_index in range(table_count):
        name = reader.text32()
        if not name:
            raise FormatError(f"table {table_index} has an empty name")
        if name.casefold() in names:
            raise FormatError(f"duplicate table name {name!r}")
        names.add(name.casefold())
        reader.u64()
        generation = max(generation, reader.u64())
        column_count = reader.u32()
        if column_count == 0 or column_count > MAX_COLUMNS:
            raise FormatError(f"table {name!r} has invalid column count")

        columns: list[Column] = []
        column_names: set[str] = set()
        for column_index in range(column_count):
            column_name = reader.text32()
            if not column_name:
                raise FormatError(
                    f"column {column_index} in {name!r} has an empty name"
                )
            folded = column_name.casefold()
            if folded in column_names:
                raise FormatError(
                    f"table {name!r} has duplicate column {column_name!r}"
                )
            column_names.add(folded)
            type_code = reader.u8()
            nullable = reader.u8()
            primary = reader.u8()
            unique = reader.u8()
            if type_code < 1 or type_code > 5:
                raise FormatError(f"column {column_name!r} has invalid type")
            if any(flag > 1 for flag in (nullable, primary, unique)):
                raise FormatError(f"column {column_name!r} has invalid flags")
            columns.append(
                Column(
                    name=column_name,
                    type_code=type_code,
                    nullable=bool(nullable),
                    primary_key=bool(primary),
                    unique=bool(unique),
                )
            )

        row_count = reader.u64()
        total_rows += row_count
        if row_count > MAX_ROWS or total_rows > MAX_ROWS:
            raise FormatError("database row count exceeds resource limit")
        rows: list[tuple[Any, ...]] = []
        for row_index in range(row_count):
            record_size = reader.u32()
            if record_size > MAX_VALUE_BYTES:
                raise FormatError(
                    f"row {row_index} in {name!r} exceeds record limit"
                )
            rows.append(_decode_record(reader.take(record_size), column_count))
        tables.append(Table(name, tuple(columns), tuple(rows)))

    if reader.remaining:
        raise FormatError(f"{path}: {reader.remaining} trailing bytes")
    if total_rows != declared_rows:
        raise FormatError(f"{path}: declared row count does not match records")
    return DatabaseImage(
        version=version,
        generation=generation,
        flags=0,
        tables=tuple(tables),
        digest=hashlib.sha256(data).hexdigest(),
    )


def _canonical_value(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _row_counter(table: Table) -> Counter[str]:
    return Counter(
        json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        for row in table.rows
    )


def compare_images(left: DatabaseImage, right: DatabaseImage) -> dict[str, Any]:
    report: dict[str, Any] = {
        "identical_bytes": left.digest == right.digest,
        "left_generation": left.generation,
        "right_generation": right.generation,
        "added_tables": [],
        "removed_tables": [],
        "changed_tables": [],
    }
    left_tables = {table.name.casefold(): table for table in left.tables}
    right_tables = {table.name.casefold(): table for table in right.tables}
    report["added_tables"] = sorted(
        right_tables[key].name for key in right_tables.keys() - left_tables.keys()
    )
    report["removed_tables"] = sorted(
        left_tables[key].name for key in left_tables.keys() - right_tables.keys()
    )

    for key in sorted(left_tables.keys() & right_tables.keys()):
        left_table = left_tables[key]
        right_table = right_tables[key]
        change: dict[str, Any] = {"table": left_table.name}
        if left_table.columns != right_table.columns:
            change["schema_changed"] = True
            change["left_columns"] = [
                dataclasses.asdict(column) for column in left_table.columns
            ]
            change["right_columns"] = [
                dataclasses.asdict(column) for column in right_table.columns
            ]
        left_rows = _row_counter(left_table)
        right_rows = _row_counter(right_table)
        removed = left_rows - right_rows
        added = right_rows - left_rows
        if removed:
            change["removed_rows"] = [
                {"row": json.loads(row), "count": count}
                for row, count in sorted(removed.items())
            ]
        if added:
            change["added_rows"] = [
                {"row": json.loads(row), "count": count}
                for row, count in sorted(added.items())
            ]
        if len(change) > 1:
            report["changed_tables"].append(change)
    report["logically_equal"] = not (
        report["added_tables"]
        or report["removed_tables"]
        or report["changed_tables"]
    )
    return report


def image_summary(image: DatabaseImage) -> dict[str, Any]:
    type_names = {
        0: "null",
        1: "integer",
        2: "real",
        3: "text",
        4: "boolean",
        5: "blob",
    }
    tables = []
    for table in image.tables:
        null_counts = [0] * len(table.columns)
        approximate_bytes = [0] * len(table.columns)
        for row in table.rows:
            for index, value in enumerate(row):
                if value is None:
                    null_counts[index] += 1
                else:
                    approximate_bytes[index] += len(_canonical_value(value))
        tables.append(
            {
                "name": table.name,
                "rows": len(table.rows),
                "columns": [
                    {
                        **dataclasses.asdict(column),
                        "type": type_names.get(column.type_code, "unknown"),
                        "null_values": null_counts[index],
                        "approximate_value_bytes": approximate_bytes[index],
                    }
                    for index, column in enumerate(table.columns)
                ],
            }
        )
    return {
        "version": image.version,
        "generation": image.generation,
        "flags": image.flags,
        "sha256": image.digest,
        "table_count": len(image.tables),
        "row_count": sum(len(table.rows) for table in image.tables),
        "tables": tables,
    }


def _print_human_diff(report: dict[str, Any]) -> None:
    print(f"byte-identical: {report['identical_bytes']}")
    print(f"logically equal: {report['logically_equal']}")
    print(
        "generation: "
        f"{report['left_generation']} -> {report['right_generation']}"
    )
    for table in report["added_tables"]:
        print(f"+ table {table}")
    for table in report["removed_tables"]:
        print(f"- table {table}")
    for change in report["changed_tables"]:
        print(f"~ table {change['table']}")
        if change.get("schema_changed"):
            print("  schema changed")
        for item in change.get("removed_rows", []):
            print(f"  - {item['count']} x {item['row']}")
        for item in change.get("added_rows", []):
            print(f"  + {item['count']} x {item['row']}")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    inspect_parser = subparsers.add_parser("inspect", help="inspect one image")
    inspect_parser.add_argument("database", type=Path)
    inspect_parser.add_argument("--json", action="store_true")
    compare_parser = subparsers.add_parser("compare", help="compare two images")
    compare_parser.add_argument("left", type=Path)
    compare_parser.add_argument("right", type=Path)
    compare_parser.add_argument("--json", action="store_true")
    validate_parser = subparsers.add_parser("validate", help="validate images")
    validate_parser.add_argument("databases", type=Path, nargs="+")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "inspect":
            summary = image_summary(parse_database(arguments.database))
            if arguments.json:
                print(json.dumps(summary, indent=2, ensure_ascii=False))
            else:
                print(
                    f"{arguments.database}: {summary['table_count']} tables, "
                    f"{summary['row_count']} rows, generation "
                    f"{summary['generation']}"
                )
                for table in summary["tables"]:
                    print(
                        f"  {table['name']}: {table['rows']} rows, "
                        f"{len(table['columns'])} columns"
                    )
            return 0
        if arguments.command == "compare":
            report = compare_images(
                parse_database(arguments.left), parse_database(arguments.right)
            )
            if arguments.json:
                print(json.dumps(report, indent=2, ensure_ascii=False))
            else:
                _print_human_diff(report)
            return 0 if report["logically_equal"] else 1
        for database in arguments.databases:
            image = parse_database(database)
            print(
                f"OK {database}: {len(image.tables)} tables, "
                f"{sum(len(table.rows) for table in image.tables)} rows"
            )
        return 0
    except (OSError, FormatError, UnicodeError) as error:
        print(f"database_diff: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
