/*
 * Offline inspector for QueryForge write-ahead logs.
 *
 * The implementation is independent from the database inspector so a damaged
 * log can be diagnosed without opening its associated database.
 */
package queryforge.tools;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.EnumMap;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.CRC32;

public final class QueryForgeWalInspector {
    private static final byte[] SIGNATURE = {
        'Q', 'F', 'W', 'A', 'L', '1', 0, 0
    };
    private static final int HEADER_BYTES = 16;
    private static final int MINIMUM_RECORD_BYTES = 40;
    private static final long MAX_FILE_BYTES = 64L * 1024L * 1024L;
    private static final int MAX_RECORD_BYTES = 4 * 1024 * 1024;
    private static final int MAX_RECORDS = 1_000_000;

    private QueryForgeWalInspector() {
    }

    public static final class FormatException extends Exception {
        private final int offset;

        FormatException(String message, int offset) {
            super(message + " at offset " + offset);
            this.offset = offset;
        }

        public int offset() {
            return offset;
        }
    }

    public enum RecordType {
        BEGIN,
        PAGE_BEFORE,
        PAGE_AFTER,
        ROW_INSERT,
        ROW_UPDATE,
        ROW_DELETE,
        CATALOG_CHANGE,
        COMMIT,
        ROLLBACK,
        CHECKPOINT;

        static RecordType fromCode(int code, int offset)
            throws FormatException {
            if (code < 1 || code > values().length) {
                throw new FormatException(
                    "invalid WAL record type " + code,
                    offset
                );
            }
            return values()[code - 1];
        }
    }

    public record Record(
        RecordType type,
        int flags,
        long lsn,
        long transactionId,
        long pageId,
        long generation,
        byte[] payload,
        int offset,
        int encodedBytes
    ) {
        Record {
            payload = payload.clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }
    }

    public record TransactionSummary(
        long transactionId,
        long firstLsn,
        long lastLsn,
        int pageImages,
        long payloadBytes,
        String outcome
    ) {
    }

    public record Wal(
        int version,
        int flags,
        long startingLsn,
        List<Record> records,
        List<TransactionSummary> transactions,
        List<String> warnings
    ) {
        Wal {
            records = List.copyOf(records);
            transactions = List.copyOf(transactions);
            warnings = List.copyOf(warnings);
        }
    }

    private static final class Reader {
        private final ByteBuffer buffer;
        private final int origin;

        Reader(byte[] data) {
            this(data, 0);
        }

        Reader(byte[] data, int origin) {
            this.buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            this.origin = origin;
        }

        int offset() {
            return origin + buffer.position();
        }

        int remaining() {
            return buffer.remaining();
        }

        void require(int count) throws FormatException {
            if (count < 0 || count > buffer.remaining()) {
                throw new FormatException(
                    "truncated WAL: need " + count
                        + " bytes, have " + buffer.remaining(),
                    offset()
                );
            }
        }

        byte[] bytes(int count) throws FormatException {
            require(count);
            byte[] result = new byte[count];
            buffer.get(result);
            return result;
        }

        int unsignedShort() throws FormatException {
            require(2);
            return buffer.getShort() & 0xffff;
        }

        long unsignedInt() throws FormatException {
            require(4);
            return Integer.toUnsignedLong(buffer.getInt());
        }

        long signedLong() throws FormatException {
            require(8);
            return buffer.getLong();
        }
    }

    private static final class MutableTransaction {
        final long transactionId;
        long firstLsn = Long.MAX_VALUE;
        long lastLsn = Long.MIN_VALUE;
        int pageImages;
        long payloadBytes;
        boolean began;
        boolean committed;
        boolean aborted;

        MutableTransaction(long transactionId) {
            this.transactionId = transactionId;
        }

        void accept(Record record) {
            firstLsn = Math.min(firstLsn, record.lsn());
            lastLsn = Math.max(lastLsn, record.lsn());
            switch (record.type()) {
                case BEGIN -> began = true;
                case PAGE_BEFORE, PAGE_AFTER -> {
                    pageImages++;
                    payloadBytes += record.payload().length;
                }
                case COMMIT -> committed = true;
                case ROLLBACK -> aborted = true;
                case ROW_INSERT, ROW_UPDATE, ROW_DELETE, CATALOG_CHANGE -> {
                    payloadBytes += record.payload().length;
                }
                case CHECKPOINT -> {
                    // Checkpoints normally use transaction id zero.
                }
            }
        }

        String outcome() {
            if (committed && aborted) {
                return "conflicting";
            }
            if (committed) {
                return "committed";
            }
            if (aborted) {
                return "aborted";
            }
            return "incomplete";
        }

        TransactionSummary freeze() {
            return new TransactionSummary(
                transactionId,
                firstLsn == Long.MAX_VALUE ? 0 : firstLsn,
                lastLsn == Long.MIN_VALUE ? 0 : lastLsn,
                pageImages,
                payloadBytes,
                outcome()
            );
        }
    }

    private static long crc32(byte[] bytes, int offset, int length) {
        CRC32 crc = new CRC32();
        crc.update(bytes, offset, length);
        return crc.getValue();
    }

    private static long readUnsignedInt(byte[] bytes, int offset) {
        return Integer.toUnsignedLong(
            ByteBuffer.wrap(bytes, offset, 4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .getInt()
        );
    }

    private static void expectSignature(Reader reader)
        throws FormatException {
        int offset = reader.offset();
        if (!java.util.Arrays.equals(reader.bytes(8), SIGNATURE)) {
            throw new FormatException("invalid WAL signature", offset);
        }
    }

    private static Record parseRecord(
        byte[] data,
        int offset,
        int length,
        long previousLsn
    ) throws FormatException {
        if (length < MINIMUM_RECORD_BYTES || length > MAX_RECORD_BYTES) {
            throw new FormatException(
                "invalid WAL record length " + length,
                offset
            );
        }
        if (offset < 0 || length > data.length - offset) {
            throw new FormatException("truncated WAL record", offset);
        }
        long expectedChecksum = readUnsignedInt(data, offset + length - 4);
        long actualChecksum = crc32(data, offset + 4, length - 8);
        if (expectedChecksum != actualChecksum) {
            throw new FormatException(
                String.format(
                    Locale.ROOT,
                    "record checksum mismatch: %08x != %08x",
                    expectedChecksum,
                    actualChecksum
                ),
                offset + length - 4
            );
        }
        Reader reader = new Reader(
            java.util.Arrays.copyOfRange(data, offset + 4, offset + length - 4),
            offset + 4
        );
        int typeOffset = reader.offset();
        RecordType type = RecordType.fromCode(
            reader.unsignedShort(),
            typeOffset
        );
        int flags = reader.unsignedShort();
        long lsn = reader.signedLong();
        long transactionId = reader.signedLong();
        long pageId = reader.unsignedInt();
        long generation = reader.unsignedInt();
        long payloadLength = reader.unsignedInt();
        if (payloadLength > MAX_RECORD_BYTES) {
            throw new FormatException(
                "WAL payload exceeds resource limit",
                reader.offset() - 4
            );
        }
        if (payloadLength != reader.remaining()) {
            throw new FormatException(
                "WAL payload length does not match record",
                reader.offset() - 4
            );
        }
        if (lsn <= previousLsn) {
            throw new FormatException(
                "WAL sequence numbers are not increasing",
                typeOffset + 4
            );
        }
        if ((type == RecordType.PAGE_BEFORE || type == RecordType.PAGE_AFTER)
            && pageId == 0xffffffffL) {
            throw new FormatException(
                "page-image record has invalid page id",
                typeOffset + 20
            );
        }
        return new Record(
            type,
            flags,
            lsn,
            transactionId,
            pageId,
            generation,
            reader.bytes((int) payloadLength),
            offset,
            length
        );
    }

    public static Wal read(Path path)
        throws IOException, FormatException {
        long fileSize = Files.size(path);
        if (fileSize > MAX_FILE_BYTES) {
            throw new FormatException("WAL exceeds resource limit", 0);
        }
        byte[] data = Files.readAllBytes(path);
        if (data.length < HEADER_BYTES) {
            throw new FormatException("truncated WAL header", 0);
        }
        Reader header = new Reader(
            java.util.Arrays.copyOfRange(data, 0, HEADER_BYTES)
        );
        expectSignature(header);
        long version = header.unsignedInt();
        long declaredRecords = header.unsignedInt();
        long startingLsn = 1;
        if (version != 1) {
            throw new FormatException(
                "unsupported WAL version " + version,
                8
            );
        }

        List<Record> records = new ArrayList<>();
        int position = HEADER_BYTES;
        long previousLsn = startingLsn - 1;
        while (records.size() < declaredRecords) {
            if (records.size() >= MAX_RECORDS) {
                throw new FormatException(
                    "WAL record count exceeds resource limit",
                    position
                );
            }
            if (data.length - position < 4) {
                throw new FormatException(
                    "truncated WAL record length",
                    position
                );
            }
            long recordLength = readUnsignedInt(data, position);
            if (recordLength > Integer.MAX_VALUE) {
                throw new FormatException(
                    "WAL record length is not representable",
                    position
                );
            }
            Record record = parseRecord(
                data,
                position,
                (int) recordLength,
                previousLsn
            );
            records.add(record);
            previousLsn = record.lsn();
            position += (int) recordLength;
        }
        if (position != data.length) {
            throw new FormatException("trailing WAL data", position);
        }

        Map<Long, MutableTransaction> transactions = new LinkedHashMap<>();
        List<String> warnings = new ArrayList<>();
        for (Record record : records) {
            if (record.type() == RecordType.CHECKPOINT) {
                if (record.transactionId() != 0) {
                    warnings.add(
                        "checkpoint LSN " + record.lsn()
                            + " has nonzero transaction id"
                    );
                }
                continue;
            }
            MutableTransaction transaction = transactions.computeIfAbsent(
                record.transactionId(),
                MutableTransaction::new
            );
            if (record.type() != RecordType.BEGIN && !transaction.began) {
                warnings.add(
                    "transaction " + record.transactionId()
                        + " has " + record.type()
                        + " before BEGIN at LSN " + record.lsn()
                );
            }
            if (transaction.committed || transaction.aborted) {
                warnings.add(
                    "transaction " + record.transactionId()
                        + " has a record after completion at LSN "
                        + record.lsn()
                );
            }
            transaction.accept(record);
        }
        List<TransactionSummary> summaries = transactions.values()
            .stream()
            .map(MutableTransaction::freeze)
            .toList();
        for (TransactionSummary summary : summaries) {
            if (summary.outcome().equals("incomplete")) {
                warnings.add(
                    "transaction " + summary.transactionId()
                        + " is incomplete"
                );
            }
            if (summary.outcome().equals("conflicting")) {
                warnings.add(
                    "transaction " + summary.transactionId()
                        + " has both COMMIT and ROLLBACK"
                );
            }
        }
        return new Wal(
            (int) version,
            0,
            startingLsn,
            records,
            summaries,
            warnings
        );
    }

    private static String escapeJson(String value) {
        StringBuilder output = new StringBuilder("\"");
        for (int index = 0; index < value.length(); index++) {
            char character = value.charAt(index);
            switch (character) {
                case '"' -> output.append("\\\"");
                case '\\' -> output.append("\\\\");
                case '\n' -> output.append("\\n");
                case '\r' -> output.append("\\r");
                case '\t' -> output.append("\\t");
                default -> {
                    if (character < 0x20) {
                        output.append(
                            String.format(
                                Locale.ROOT,
                                "\\u%04x",
                                (int) character
                            )
                        );
                    } else {
                        output.append(character);
                    }
                }
            }
        }
        return output.append('"').toString();
    }

    public static String toJson(Wal wal) {
        StringBuilder output = new StringBuilder();
        output.append("{\n");
        output.append("  \"version\": ").append(wal.version()).append(",\n");
        output.append("  \"flags\": ").append(wal.flags()).append(",\n");
        output.append("  \"starting_lsn\": ")
            .append(wal.startingLsn())
            .append(",\n");
        output.append("  \"record_count\": ")
            .append(wal.records().size())
            .append(",\n");
        output.append("  \"transactions\": [\n");
        for (int index = 0; index < wal.transactions().size(); index++) {
            TransactionSummary item = wal.transactions().get(index);
            output.append("    {\"id\": ")
                .append(item.transactionId())
                .append(", \"first_lsn\": ")
                .append(item.firstLsn())
                .append(", \"last_lsn\": ")
                .append(item.lastLsn())
                .append(", \"page_images\": ")
                .append(item.pageImages())
                .append(", \"payload_bytes\": ")
                .append(item.payloadBytes())
                .append(", \"outcome\": ")
                .append(escapeJson(item.outcome()))
                .append("}");
            if (index + 1 < wal.transactions().size()) {
                output.append(',');
            }
            output.append('\n');
        }
        output.append("  ],\n");
        output.append("  \"warnings\": [");
        for (int index = 0; index < wal.warnings().size(); index++) {
            if (index != 0) {
                output.append(", ");
            }
            output.append(escapeJson(wal.warnings().get(index)));
        }
        return output.append("]\n}\n").toString();
    }

    private static void printHuman(Path path, Wal wal, boolean records) {
        EnumMap<RecordType, Integer> counts = new EnumMap<>(RecordType.class);
        long payloadBytes = 0;
        for (Record record : wal.records()) {
            counts.merge(record.type(), 1, Integer::sum);
            payloadBytes += record.payload().length;
        }
        System.out.printf(
            Locale.ROOT,
            "%s: version=%d records=%d transactions=%d payload=%d bytes%n",
            path,
            wal.version(),
            wal.records().size(),
            wal.transactions().size(),
            payloadBytes
        );
        for (RecordType type : RecordType.values()) {
            System.out.printf(
                Locale.ROOT,
                "  %-12s %d%n",
                type,
                counts.getOrDefault(type, 0)
            );
        }
        for (TransactionSummary transaction : wal.transactions()) {
            System.out.printf(
                Locale.ROOT,
                "  tx=%d lsn=%d..%d pages=%d payload=%d outcome=%s%n",
                transaction.transactionId(),
                transaction.firstLsn(),
                transaction.lastLsn(),
                transaction.pageImages(),
                transaction.payloadBytes(),
                transaction.outcome()
            );
        }
        for (String warning : wal.warnings()) {
            System.out.println("  warning: " + warning);
        }
        if (records) {
            for (Record record : wal.records()) {
                System.out.printf(
                    Locale.ROOT,
                    "  @%-8d lsn=%-8d tx=%-6d %-12s page=%-6d "
                        + "gen=%-6d payload=%d%n",
                    record.offset(),
                    record.lsn(),
                    record.transactionId(),
                    record.type(),
                    record.pageId(),
                    record.generation(),
                    record.payload().length
                );
            }
        }
    }

    private static void usage() {
        System.err.println(
            "Usage: QueryForgeWalInspector [--json] [--records] WAL..."
        );
    }

    public static void main(String[] arguments) {
        boolean json = false;
        boolean records = false;
        List<Path> paths = new ArrayList<>();
        for (String argument : arguments) {
            switch (argument) {
                case "--json" -> json = true;
                case "--records" -> records = true;
                default -> {
                    if (argument.startsWith("-")) {
                        usage();
                        System.exit(2);
                    }
                    paths.add(Path.of(argument));
                }
            }
        }
        if (paths.isEmpty() || (json && paths.size() != 1)) {
            usage();
            System.exit(2);
        }
        try {
            for (Path path : paths) {
                Wal wal = read(path);
                if (json) {
                    System.out.print(toJson(wal));
                } else {
                    printHuman(path, wal, records);
                }
            }
        } catch (IOException | FormatException error) {
            System.err.println(
                "QueryForgeWalInspector: " + error.getMessage()
            );
            System.exit(1);
        }
    }
}
