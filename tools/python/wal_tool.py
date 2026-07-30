#!/usr/bin/env python3
"""Audit, summarize, truncate, and concatenate QueryForge WAL streams."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys
from dataclasses import asdict
from typing import Iterable

from queryforge_tool import FormatError, WalRecord, build_wal, parse_wal

TYPE_NAMES = {
    1: "begin", 2: "page_before", 3: "page_after", 4: "row_insert",
    5: "row_update", 6: "row_delete", 7: "catalog_change", 8: "commit",
    9: "rollback", 10: "checkpoint",
}


def summarize(records: list[WalRecord]) -> dict[str, object]:
    transactions: dict[int, list[WalRecord]] = collections.defaultdict(list)
    type_counts: dict[str, int] = collections.Counter()
    for record in records:
        transactions[record.transaction].append(record)
        type_counts[TYPE_NAMES.get(record.type, "unknown")] += 1
    transaction_reports = []
    for identifier, values in sorted(transactions.items()):
        types = [record.type for record in values]
        state = ("committed" if 8 in types else
                 "rolled_back" if 9 in types else "incomplete")
        transaction_reports.append({
            "identifier": identifier, "state": state,
            "first_lsn": values[0].lsn, "last_lsn": values[-1].lsn,
            "record_count": len(values),
            "payload_bytes": sum(len(record.payload) for record in values),
            "pages": sorted({record.page for record in values
                             if record.page}),
        })
    return {
        "record_count": len(records),
        "first_lsn": records[0].lsn if records else 0,
        "last_lsn": records[-1].lsn if records else 0,
        "payload_bytes": sum(len(record.payload) for record in records),
        "type_counts": dict(type_counts),
        "transactions": transaction_reports,
        "incomplete_transactions": sum(
            item["state"] == "incomplete" for item in transaction_reports),
    }


def concatenate(paths: list[pathlib.Path]) -> list[WalRecord]:
    output: list[WalRecord] = []
    next_lsn = 1
    for path in paths:
        records = parse_wal(path.read_bytes())
        for record in records:
            output.append(WalRecord(record.type, next_lsn,
                                    record.transaction, record.page,
                                    record.generation, record.payload))
            next_lsn += 1
    return output


def main(arguments: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wal", type=pathlib.Path, nargs="+")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--through-lsn", type=int)
    parser.add_argument("--records", action="store_true")
    options = parser.parse_args(arguments)
    try:
        records = concatenate(options.wal)
        if options.through_lsn is not None:
            if options.through_lsn < 0:
                raise ValueError("--through-lsn cannot be negative")
            records = [record for record in records
                       if record.lsn <= options.through_lsn]
        report = summarize(records)
        if options.records:
            report["records"] = [
                {**asdict(record), "type_name": TYPE_NAMES.get(record.type),
                 "payload": record.payload.hex()}
                for record in records
            ]
        print(json.dumps(report, indent=2, sort_keys=True))
        if options.output:
            options.output.write_bytes(build_wal(records))
    except (OSError, ValueError, FormatError) as error:
        print(f"wal_tool: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
