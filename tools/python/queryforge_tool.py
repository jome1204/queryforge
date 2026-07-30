#!/usr/bin/env python3
"""Generate and inspect QueryForge database images, WALs, and fuzz corpora."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import struct
import sys
import zlib
from dataclasses import asdict, dataclass, field
from typing import Iterable

DATABASE_HEADER = struct.Struct("<8sIIQII")
RECORD_HEADER = struct.Struct("<4sQQBH")
WAL_HEADER = struct.Struct("<8sII")
WAL_RECORD = struct.Struct("<IHHQQIII")
MAX_FILE = 256 * 1024 * 1024
MAX_RECORD = 4 * 1024 * 1024
MAX_ROWS = 1_000_000
MAX_COLUMNS = 1024


class FormatError(ValueError):
    pass


@dataclass
class Column:
    name: str
    type: int
    nullable: bool
    primary_key: bool
    unique: bool


@dataclass
class Record:
    row_id: int
    generation: int
    deleted: bool
    values: list[object]


@dataclass
class Table:
    name: str
    identifier: int
    generation: int
    columns: list[Column]
    records: list[Record] = field(default_factory=list)


@dataclass
class WalRecord:
    type: int
    lsn: int
    transaction: int
    page: int
    generation: int
    payload: bytes


def bounded(offset: int, length: int, size: int, label: str) -> None:
    if offset < 0 or length < 0 or offset > size or length > size - offset:
        raise FormatError(f"{label} range exceeds source")


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.position = 0

    @property
    def remaining(self) -> int:
        return len(self.data) - self.position

    def take(self, count: int, label: str) -> bytes:
        bounded(self.position, count, len(self.data), label)
        output = self.data[self.position:self.position + count]
        self.position += count
        return output

    def u8(self, label: str) -> int:
        return self.take(1, label)[0]

    def u16(self, label: str) -> int:
        return struct.unpack("<H", self.take(2, label))[0]

    def u32(self, label: str) -> int:
        return struct.unpack("<I", self.take(4, label))[0]

    def u64(self, label: str) -> int:
        return struct.unpack("<Q", self.take(8, label))[0]

    def text(self, maximum: int, label: str) -> str:
        length = self.u32(f"{label} length")
        if length > maximum:
            raise FormatError(f"{label} exceeds limit")
        return self.take(length, label).decode("utf-8", "replace")


def encode_value(value: object) -> bytes:
    if value is None:
        return b"\0"
    if isinstance(value, bool):
        return b"\x04" + bytes((int(value),))
    if isinstance(value, int):
        return b"\x01" + struct.pack("<q", value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("real value must be finite")
        return b"\x02" + struct.pack("<d", value)
    if isinstance(value, str):
        encoded = value.encode()
        return b"\x03" + struct.pack("<I", len(encoded)) + encoded
    if isinstance(value, bytes):
        return b"\x05" + struct.pack("<I", len(value)) + value
    raise TypeError(f"unsupported record value: {type(value).__name__}")


def decode_value(reader: Reader) -> object:
    value_type = reader.u8("value type")
    if value_type == 0:
        return None
    if value_type == 1:
        return struct.unpack("<q", reader.take(8, "integer"))[0]
    if value_type == 2:
        value = struct.unpack("<d", reader.take(8, "real"))[0]
        if not math.isfinite(value):
            raise FormatError("record real is nonfinite")
        return value
    if value_type == 3:
        return reader.text(1024 * 1024, "text")
    if value_type == 4:
        value = reader.u8("boolean")
        if value > 1:
            raise FormatError("record boolean is invalid")
        return bool(value)
    if value_type == 5:
        length = reader.u32("blob length")
        if length > MAX_RECORD:
            raise FormatError("record blob exceeds limit")
        return reader.take(length, "blob")
    raise FormatError(f"unknown record value type {value_type}")


def encode_record(record: Record) -> bytes:
    output = bytearray(RECORD_HEADER.pack(
        b"QFR1", record.row_id, record.generation,
        int(record.deleted), len(record.values)))
    for value in record.values:
        output.extend(encode_value(value))
    if len(output) + 4 > MAX_RECORD:
        raise ValueError("encoded record exceeds limit")
    output.extend(struct.pack("<I", zlib.crc32(output) & 0xffffffff))
    return bytes(output)


def decode_record(data: bytes) -> Record:
    if len(data) < 27 or data[:4] != b"QFR1":
        raise FormatError("record header is invalid")
    if zlib.crc32(data[:-4]) & 0xffffffff != struct.unpack_from(
            "<I", data, len(data) - 4)[0]:
        raise FormatError("record checksum mismatch")
    signature, row_id, generation, deleted, columns = \
        RECORD_HEADER.unpack_from(data)
    del signature
    if columns > MAX_COLUMNS or deleted > 1:
        raise FormatError("record fields exceed limits")
    reader = Reader(data[RECORD_HEADER.size:-4])
    values = [decode_value(reader) for _ in range(columns)]
    if reader.remaining:
        raise FormatError("record has trailing bytes")
    return Record(row_id, generation, bool(deleted), values)


def append_text(output: bytearray, value: str) -> None:
    encoded = value.encode()
    output.extend(struct.pack("<I", len(encoded)))
    output.extend(encoded)


def build_database(tables: list[Table]) -> bytes:
    if not 1 <= len(tables) <= 4096:
        raise ValueError("table count exceeds limits")
    total_rows = sum(len(table.records) for table in tables)
    output = bytearray(DATABASE_HEADER.size)
    for table in tables:
        append_text(output, table.name)
        output.extend(struct.pack("<QQI", table.identifier, table.generation,
                                  len(table.columns)))
        for column in table.columns:
            append_text(output, column.name)
            output.extend(bytes((column.type, int(column.nullable),
                                 int(column.primary_key), int(column.unique))))
        output.extend(struct.pack("<Q", len(table.records)))
        for record in table.records:
            encoded = encode_record(record)
            output.extend(struct.pack("<I", len(encoded)))
            output.extend(encoded)
    checksum = zlib.crc32(output[32:]) & 0xffffffff
    DATABASE_HEADER.pack_into(output, 0, b"QFENG1\0\0", 1, len(tables),
                              total_rows, 4096, checksum)
    if len(output) > MAX_FILE:
        raise ValueError("database image exceeds file limit")
    return bytes(output)


def parse_database(data: bytes) -> list[Table]:
    if len(data) < DATABASE_HEADER.size:
        raise FormatError("database header is truncated")
    signature, version, table_count, row_count, page_size, checksum = \
        DATABASE_HEADER.unpack_from(data)
    if signature != b"QFENG1\0\0" or version != 1 or page_size != 4096:
        raise FormatError("database signature or version is invalid")
    if table_count > 4096 or row_count > MAX_ROWS:
        raise FormatError("database counts exceed limits")
    if zlib.crc32(data[32:]) & 0xffffffff != checksum:
        raise FormatError("database checksum mismatch")
    reader = Reader(data[32:])
    tables: list[Table] = []
    actual_rows = 0
    identifiers: set[int] = set()
    for _ in range(table_count):
        name = reader.text(256, "table name")
        identifier = reader.u64("table identifier")
        generation = reader.u64("table generation")
        column_count = reader.u32("column count")
        if identifier == 0 or identifier in identifiers:
            raise FormatError("table identifier is invalid or duplicated")
        if not 1 <= column_count <= MAX_COLUMNS:
            raise FormatError("column count exceeds limits")
        identifiers.add(identifier)
        columns: list[Column] = []
        names: set[str] = set()
        for _ in range(column_count):
            column_name = reader.text(256, "column name")
            values = reader.take(4, "column properties")
            data_type, nullable, primary, unique = values
            if not 1 <= data_type <= 5 or \
                    any(value > 1 for value in values[1:]):
                raise FormatError("column properties are invalid")
            if column_name.upper() in names:
                raise FormatError("column name is duplicated")
            names.add(column_name.upper())
            columns.append(Column(column_name, data_type, bool(nullable),
                                  bool(primary), bool(unique)))
        table_rows = reader.u64("table row count")
        if table_rows > MAX_ROWS - actual_rows:
            raise FormatError("table row count exceeds limits")
        records: list[Record] = []
        for _ in range(table_rows):
            length = reader.u32("record length")
            if length > MAX_RECORD:
                raise FormatError("record length exceeds limit")
            record = decode_record(reader.take(length, "record"))
            if len(record.values) != len(columns):
                raise FormatError("record width differs from schema")
            records.append(record)
            actual_rows += 1
        tables.append(Table(name, identifier, generation, columns, records))
    if reader.remaining or actual_rows != row_count:
        raise FormatError("database trailing bytes or row count mismatch")
    return tables


def build_wal(records: list[WalRecord]) -> bytes:
    output = bytearray(WAL_HEADER.pack(b"QFWAL1\0\0", 1, len(records)))
    previous = 0
    for record in records:
        if record.lsn <= previous or len(record.payload) > MAX_RECORD:
            raise ValueError("WAL LSN or payload is invalid")
        length = 40 + len(record.payload)
        start = len(output)
        output.extend(WAL_RECORD.pack(length, record.type, 0, record.lsn,
                                      record.transaction, record.page,
                                      record.generation, len(record.payload)))
        output.extend(record.payload)
        output.extend(struct.pack(
            "<I", zlib.crc32(output[start + 4:]) & 0xffffffff))
        previous = record.lsn
    return bytes(output)


def parse_wal(data: bytes) -> list[WalRecord]:
    if len(data) < WAL_HEADER.size:
        raise FormatError("WAL header is truncated")
    signature, version, count = WAL_HEADER.unpack_from(data)
    if signature != b"QFWAL1\0\0" or version != 1 or count > MAX_ROWS:
        raise FormatError("WAL header is invalid")
    position = WAL_HEADER.size
    records: list[WalRecord] = []
    previous = 0
    for _ in range(count):
        bounded(position, 40, len(data), "WAL record header")
        fields = WAL_RECORD.unpack_from(data, position)
        length, record_type, flags, lsn, transaction, page, generation, \
            payload_size = fields
        if not 1 <= record_type <= 10 or flags != 0 or \
                length != payload_size + 40:
            raise FormatError("WAL record fields are inconsistent")
        bounded(position, length, len(data), "WAL record")
        stored, = struct.unpack_from("<I", data, position + length - 4)
        calculated = zlib.crc32(
            data[position + 4:position + length - 4]) & 0xffffffff
        if stored != calculated:
            raise FormatError("WAL record checksum mismatch")
        if lsn <= previous:
            raise FormatError("WAL LSN order is invalid")
        payload = data[position + 36:position + 36 + payload_size]
        records.append(WalRecord(record_type, lsn, transaction, page,
                                 generation, payload))
        previous = lsn
        position += length
    if position != len(data):
        raise FormatError("WAL contains trailing bytes")
    return records


def seed_tables(rows: int, large: bool = False) -> list[Table]:
    users = Table("users", 1, 1, [
        Column("id", 1, False, True, True),
        Column("name", 3, False, False, False),
        Column("score", 2, True, False, False),
        Column("active", 4, False, False, False),
    ])
    notes = Table("notes", 2, 1, [
        Column("id", 1, False, True, True),
        Column("user_id", 1, False, False, False),
        Column("body", 3, True, False, False),
    ])
    for index in range(rows):
        name = ("user-" + str(index) + "-" + "x" * 2048
                if large and index == rows - 1 else f"user-{index}")
        users.records.append(
            Record(index + 1, 1, False,
                   [index + 1, name, index * 1.25,
                    index % 2 == 0]))
        notes.records.append(
            Record(rows + index + 1, 1, False,
                   [index + 1, index + 1,
                    None if index % 3 == 0 else f"note {index}"]))
    return [users, notes]


def write_corpus(root: pathlib.Path) -> None:
    parser = root / "sql_parser_fuzzer"
    execution = root / "sql_execution_fuzzer"
    database = root / "database_file_fuzzer"
    wal = root / "wal_recovery_fuzzer"
    transaction = root / "transaction_sequence_fuzzer"
    for directory in (parser, execution, database, wal, transaction):
        directory.mkdir(parents=True, exist_ok=True)
    parser.joinpath("schema_and_rows.sql").write_text(
        "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "score REAL DEFAULT 0);"
        "INSERT INTO users VALUES(1,'Ada',9.5),(2,'Linus',NULL);"
        "UPDATE users SET score=score+1 WHERE id=1;"
        "SELECT id,name,score FROM users WHERE score>=5 ORDER BY id;",
        encoding="utf-8")
    parser.joinpath("joins_aggregates.sql").write_text(
        "SELECT u.name,COUNT(n.id) AS notes FROM users AS u "
        "LEFT JOIN notes AS n ON n.user_id=u.id "
        "WHERE u.active=TRUE GROUP BY u.name HAVING COUNT(n.id)>=0 "
        "ORDER BY u.name LIMIT 100;"
        "CREATE UNIQUE INDEX users_name ON users(name);",
        encoding="utf-8")
    parser.joinpath("transactions_nulls.sql").write_text(
        "BEGIN; INSERT INTO users(id,name,score,active) "
        "VALUES(3,'long ''quoted'' string',NULL,FALSE); "
        "UPDATE users SET score=COALESCE(score,0)+2; ROLLBACK;",
        encoding="utf-8")
    execution.joinpath("basic_workload.sql").write_text(
        "CREATE TABLE items(id INTEGER PRIMARY KEY,name TEXT,price REAL);"
        "INSERT INTO items VALUES(1,'one',1.25),(2,'two',2.5);"
        "SELECT COUNT(*),SUM(price),AVG(price) FROM items;"
        "BEGIN;UPDATE items SET price=price*2 WHERE id=2;COMMIT;"
        "SELECT * FROM items ORDER BY id;",
        encoding="utf-8")
    execution.joinpath("rollback_workload.sql").write_text(
        "CREATE TABLE log(id INTEGER PRIMARY KEY,message TEXT);"
        "BEGIN;INSERT INTO log VALUES(1,'temporary');ROLLBACK;"
        "SELECT COUNT(*) FROM log;",
        encoding="utf-8")
    database.joinpath("multi_table.qfdb").write_bytes(
        build_database(seed_tables(16)))
    database.joinpath("large_text.qfdb").write_bytes(
        build_database(seed_tables(4, True)))
    database.joinpath("deleted_rows.qfdb").write_bytes(
        build_database([
            Table("events", 1, 3,
                  [Column("id", 1, False, True, True),
                   Column("payload", 5, True, False, False)],
                  [Record(1, 1, False, [1, bytes(range(64))]),
                   Record(2, 2, True, [2, b"deleted"])])
        ]))
    committed = [
        WalRecord(1, 1, 7, 0, 0, b""),
        WalRecord(4, 2, 7, 0, 0, b"users"),
        WalRecord(8, 3, 7, 0, 0, b""),
        WalRecord(10, 4, 0, 0, 0, b""),
    ]
    incomplete = [
        WalRecord(1, 1, 9, 0, 0, b""),
        WalRecord(5, 2, 9, 0, 0, b"users"),
    ]
    base = build_database(seed_tables(3))
    wal.joinpath("committed.seed").write_bytes(
        struct.pack("<I", len(base)) + base + build_wal(committed))
    wal.joinpath("incomplete.seed").write_bytes(
        struct.pack("<I", len(base)) + base + build_wal(incomplete))
    transaction.joinpath("commit_sequence.seed").write_bytes(bytes([
        0, 2, 1, 1, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13]))
    transaction.joinpath("rollback_sequence.seed").write_bytes(bytes([
        0, 1, 2, 2, 3, 5, 1, 4, 2, 6, 8, 10, 14, 15]))


def database_summary(tables: list[Table]) -> dict[str, object]:
    return {
        "table_count": len(tables),
        "row_count": sum(len(table.records) for table in tables),
        "tables": [
            {
                "name": table.name,
                "identifier": table.identifier,
                "generation": table.generation,
                "columns": [asdict(column) for column in table.columns],
                "rows": len(table.records),
                "live_rows": sum(not record.deleted
                                 for record in table.records),
                "deleted_rows": sum(record.deleted
                                    for record in table.records),
            }
            for table in tables
        ],
    }


def main(arguments: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect")
    inspect.add_argument("database", type=pathlib.Path)
    wal = commands.add_parser("wal")
    wal.add_argument("wal", type=pathlib.Path)
    corpus = commands.add_parser("corpus")
    corpus.add_argument("root", type=pathlib.Path)
    seed = commands.add_parser("seed")
    seed.add_argument("database", type=pathlib.Path)
    seed.add_argument("--rows", type=int, default=16)
    seed.add_argument("--large", action="store_true")
    options = parser.parse_args(arguments)
    try:
        if options.command == "inspect":
            tables = parse_database(options.database.read_bytes())
            print(json.dumps(database_summary(tables), indent=2,
                             sort_keys=True))
        elif options.command == "wal":
            records = parse_wal(options.wal.read_bytes())
            print(json.dumps([
                {**asdict(record), "payload": record.payload.hex()}
                for record in records], indent=2, sort_keys=True))
        elif options.command == "corpus":
            write_corpus(options.root)
        else:
            if not 1 <= options.rows <= 100000:
                raise ValueError("row count must be between 1 and 100000")
            options.database.parent.mkdir(parents=True, exist_ok=True)
            options.database.write_bytes(
                build_database(seed_tables(options.rows, options.large)))
    except (OSError, UnicodeError, ValueError, TypeError,
            FormatError, struct.error) as error:
        print(f"queryforge_tool: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
