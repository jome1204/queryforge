/*
 * QueryForge database inspector.
 *
 * This standalone Java utility provides a second implementation of the portable
 * database image reader. It has no third-party dependencies and deliberately
 * applies stricter allocation limits before reading variable-length fields.
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
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.CRC32;

public final class QueryForgeInspector {
    private static final byte[] DATABASE_SIGNATURE = {
        'Q', 'F', 'E', 'N', 'G', '1', 0, 0
    };
    private static final byte[] RECORD_SIGNATURE = {'Q', 'F', 'R', '1'};
    private static final long MAX_FILE_BYTES = 256L * 1024L * 1024L;
    private static final int MAX_TABLES = 4096;
    private static final int MAX_COLUMNS = 1024;
    private static final long MAX_ROWS = 1_000_000L;
    private static final int MAX_RECORD_BYTES = 4 * 1024 * 1024;

    private QueryForgeInspector() {
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

    public enum ValueType {
        NULL,
        INTEGER,
        REAL,
        TEXT,
        BOOLEAN,
        BLOB;

        static ValueType fromCode(int code, int offset) throws FormatException {
            if (code < 0 || code >= values().length) {
                throw new FormatException("invalid value type " + code, offset);
            }
            return values()[code];
        }
    }

    public record Column(
        String name,
        ValueType type,
        boolean nullable,
        boolean primaryKey,
        boolean unique
    ) {
    }

    public record Cell(ValueType type, Object value) {
        public String display() {
            if (type == ValueType.NULL) {
                return "NULL";
            }
            if (type == ValueType.TEXT) {
                return "'" + value.toString().replace("'", "''") + "'";
            }
            if (type == ValueType.BLOB) {
                byte[] bytes = (byte[]) value;
                StringBuilder output = new StringBuilder("X'");
                for (byte item : bytes) {
                    output.append(String.format(Locale.ROOT, "%02x", item & 0xff));
                }
                return output.append("'").toString();
            }
            return String.valueOf(value);
        }

        public long approximateMemory() {
            if (value == null) {
                return 0;
            }
            if (value instanceof byte[] bytes) {
                return bytes.length;
            }
            if (value instanceof String text) {
                return text.length() * 2L;
            }
            return 8;
        }
    }

    public record Row(List<Cell> values) {
        Row {
            values = List.copyOf(values);
        }
    }

    public record Table(String name, List<Column> columns, List<Row> rows) {
        Table {
            columns = List.copyOf(columns);
            rows = List.copyOf(rows);
        }
    }

    public record Database(
        int version,
        int flags,
        long generation,
        List<Table> tables,
        String sha256
    ) {
        Database {
            tables = List.copyOf(tables);
        }

        public long rowCount() {
            return tables.stream().mapToLong(table -> table.rows().size()).sum();
        }

        public long approximateValueMemory() {
            return tables.stream()
                .flatMap(table -> table.rows().stream())
                .flatMap(row -> row.values().stream())
                .mapToLong(Cell::approximateMemory)
                .sum();
        }
    }

    private static final class Reader {
        private final ByteBuffer buffer;

        Reader(byte[] data) {
            buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        }

        int position() {
            return buffer.position();
        }

        int remaining() {
            return buffer.remaining();
        }

        byte[] bytes(int count) throws FormatException {
            require(count);
            byte[] result = new byte[count];
            buffer.get(result);
            return result;
        }

        int unsignedByte() throws FormatException {
            require(1);
            return buffer.get() & 0xff;
        }

        int unsignedShort() throws FormatException {
            require(2);
            return buffer.getShort() & 0xffff;
        }

        long unsignedInt() throws FormatException {
            require(4);
            return Integer.toUnsignedLong(buffer.getInt());
        }

        int signedInt() throws FormatException {
            require(4);
            return buffer.getInt();
        }

        long signedLong() throws FormatException {
            require(8);
            return buffer.getLong();
        }

        double real() throws FormatException {
            require(8);
            return buffer.getDouble();
        }

        String text16() throws FormatException {
            int length = unsignedShort();
            return decodeUtf8(bytes(length), position() - length);
        }

        String text32() throws FormatException {
            long length = unsignedInt();
            if (length > MAX_RECORD_BYTES) {
                throw new FormatException("text exceeds resource limit", position());
            }
            return decodeUtf8(bytes((int) length), position() - (int) length);
        }

        Reader slice(int count) throws FormatException {
            return new Reader(bytes(count));
        }

        private void require(int count) throws FormatException {
            if (count < 0 || count > buffer.remaining()) {
                throw new FormatException(
                    "truncated input: need " + count
                        + " bytes, have " + buffer.remaining(),
                    buffer.position()
                );
            }
        }
    }

    private static String decodeUtf8(byte[] bytes, int offset)
        throws FormatException {
        try {
            return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes))
                .toString();
        } catch (CharacterCodingException error) {
            throw new FormatException("invalid UTF-8 string", offset);
        }
    }

    private static void expectBytes(
        Reader reader,
        byte[] expected,
        String description
    ) throws FormatException {
        int offset = reader.position();
        byte[] actual = reader.bytes(expected.length);
        if (!java.util.Arrays.equals(actual, expected)) {
            throw new FormatException("invalid " + description, offset);
        }
    }

    private static void verifyChecksum(byte[] bytes, String description)
        throws FormatException {
        if (bytes.length < 4) {
            throw new FormatException("truncated " + description, 0);
        }
        int checksumOffset = bytes.length - 4;
        long expected = Integer.toUnsignedLong(
            ByteBuffer.wrap(bytes, checksumOffset, 4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .getInt()
        );
        CRC32 crc = new CRC32();
        crc.update(bytes, 0, checksumOffset);
        if (expected != crc.getValue()) {
            throw new FormatException(
                String.format(
                    Locale.ROOT,
                    "%s checksum mismatch: %08x != %08x",
                    description,
                    expected,
                    crc.getValue()
                ),
                checksumOffset
            );
        }
    }

    private static Cell readCell(Reader reader) throws FormatException {
        int typeOffset = reader.position();
        ValueType type = ValueType.fromCode(reader.unsignedByte(), typeOffset);
        return switch (type) {
            case NULL -> new Cell(type, null);
            case INTEGER -> new Cell(type, reader.signedLong());
            case REAL -> new Cell(type, reader.real());
            case TEXT -> {
                long length = reader.unsignedInt();
                if (length > MAX_RECORD_BYTES) {
                    throw new FormatException(
                        "text exceeds resource limit",
                        reader.position() - 4
                    );
                }
                yield new Cell(
                    type,
                    decodeUtf8(reader.bytes((int) length), reader.position())
                );
            }
            case BOOLEAN -> {
                int value = reader.unsignedByte();
                if (value > 1) {
                    throw new FormatException(
                        "invalid Boolean value",
                        reader.position() - 1
                    );
                }
                yield new Cell(type, value != 0);
            }
            case BLOB -> {
                long length = reader.unsignedInt();
                if (length > MAX_RECORD_BYTES) {
                    throw new FormatException(
                        "blob exceeds resource limit",
                        reader.position() - 4
                    );
                }
                yield new Cell(type, reader.bytes((int) length));
            }
        };
    }

    private static Row readRecord(byte[] record, int columnCount)
        throws FormatException {
        verifyChecksum(record, "record");
        Reader reader = new Reader(
            java.util.Arrays.copyOf(record, record.length - 4)
        );
        expectBytes(reader, RECORD_SIGNATURE, "record signature");
        reader.signedLong();
        reader.signedLong();
        int deleted = reader.unsignedByte();
        if (deleted > 1) {
            throw new FormatException("invalid record deletion flag", 20);
        }
        long valueCount = reader.unsignedShort();
        if (valueCount != columnCount) {
            throw new FormatException(
                "record field count does not match schema",
                reader.position() - 4
            );
        }
        List<Cell> cells = new ArrayList<>(columnCount);
        for (int index = 0; index < columnCount; index++) {
            cells.add(readCell(reader));
        }
        if (reader.remaining() != 0) {
            throw new FormatException(
                "trailing record data",
                reader.position()
            );
        }
        return new Row(cells);
    }

    public static Database read(Path path)
        throws IOException, FormatException {
        long fileSize = Files.size(path);
        if (fileSize > MAX_FILE_BYTES) {
            throw new FormatException("database exceeds resource limit", 0);
        }
        byte[] data = Files.readAllBytes(path);
        if (data.length < 36) {
            throw new FormatException("truncated database header", 0);
        }
        long expectedBodyChecksum = Integer.toUnsignedLong(
            ByteBuffer.wrap(data, 28, 4).order(ByteOrder.LITTLE_ENDIAN).getInt()
        );
        CRC32 bodyChecksum = new CRC32();
        bodyChecksum.update(data, 32, data.length - 32);
        if (expectedBodyChecksum != bodyChecksum.getValue()) {
            throw new FormatException("database body checksum mismatch", 28);
        }
        Reader reader = new Reader(data);
        expectBytes(reader, DATABASE_SIGNATURE, "database signature");
        long version = reader.unsignedInt();
        long tableCount = reader.unsignedInt();
        long declaredRows = reader.signedLong();
        long pageSize = reader.unsignedInt();
        reader.unsignedInt();
        if (version != 1) {
            throw new FormatException(
                "unsupported database version " + version,
                8
            );
        }
        if (tableCount > MAX_TABLES) {
            throw new FormatException("too many tables", 24);
        }
        if (pageSize != 4096) {
            throw new FormatException("unsupported page size", 24);
        }

        List<Table> tables = new ArrayList<>((int) tableCount);
        Map<String, Boolean> tableNames = new LinkedHashMap<>();
        long totalRows = 0;
        long maximumGeneration = 0;
        for (int tableIndex = 0; tableIndex < tableCount; tableIndex++) {
            int nameOffset = reader.position();
            String tableName = reader.text32();
            if (tableName.isEmpty()) {
                throw new FormatException("empty table name", nameOffset);
            }
            String tableKey = tableName.toLowerCase(Locale.ROOT);
            if (tableNames.put(tableKey, Boolean.TRUE) != null) {
                throw new FormatException(
                    "duplicate table " + tableName,
                    nameOffset
                );
            }
            reader.signedLong();
            long tableGeneration = reader.signedLong();
            maximumGeneration = Math.max(maximumGeneration, tableGeneration);
            long columnCount = reader.unsignedInt();
            if (columnCount == 0 || columnCount > MAX_COLUMNS) {
                throw new FormatException(
                    "invalid column count",
                    reader.position() - 12
                );
            }
            List<Column> columns = new ArrayList<>((int) columnCount);
            Map<String, Boolean> columnNames = new LinkedHashMap<>();
            for (int columnIndex = 0; columnIndex < columnCount; columnIndex++) {
                int columnOffset = reader.position();
                String columnName = reader.text32();
                if (columnName.isEmpty()) {
                    throw new FormatException(
                        "empty column name",
                        columnOffset
                    );
                }
                String columnKey = columnName.toLowerCase(Locale.ROOT);
                if (columnNames.put(columnKey, Boolean.TRUE) != null) {
                    throw new FormatException(
                        "duplicate column " + columnName,
                        columnOffset
                    );
                }
                ValueType type = ValueType.fromCode(
                    reader.unsignedByte(),
                    reader.position() - 1
                );
                int nullable = reader.unsignedByte();
                int primary = reader.unsignedByte();
                int unique = reader.unsignedByte();
                if (nullable > 1 || primary > 1 || unique > 1) {
                    throw new FormatException(
                        "unknown column property bits",
                        reader.position() - 1
                    );
                }
                columns.add(
                    new Column(
                        columnName,
                        type,
                        nullable != 0,
                        primary != 0,
                        unique != 0
                    )
                );
            }

            long rowCount = reader.signedLong();
            if (rowCount < 0 || rowCount > MAX_ROWS - totalRows) {
                throw new FormatException("invalid row count", reader.position() - 8);
            }
            totalRows += rowCount;
            List<Row> rows = new ArrayList<>((int) rowCount);
            for (int rowIndex = 0; rowIndex < rowCount; rowIndex++) {
                long recordSize = reader.unsignedInt();
                if (recordSize > MAX_RECORD_BYTES) {
                    throw new FormatException(
                        "record exceeds resource limit",
                        reader.position() - 4
                    );
                }
                rows.add(
                    readRecord(reader.bytes((int) recordSize), (int) columnCount)
                );
            }
            tables.add(new Table(tableName, columns, rows));
        }
        if (reader.remaining() != 0) {
            throw new FormatException(
                "trailing database data",
                reader.position()
            );
        }
        if (totalRows != declaredRows) {
            throw new FormatException("declared row count differs from records", 16);
        }
        return new Database(
            (int) version,
            (int) pageSize,
            maximumGeneration,
            tables,
            sha256(data)
        );
    }

    private static String sha256(byte[] data) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            StringBuilder result = new StringBuilder();
            for (byte item : digest.digest(data)) {
                result.append(
                    String.format(Locale.ROOT, "%02x", item & 0xff)
                );
            }
            return result.toString();
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static String jsonEscape(String text) {
        StringBuilder result = new StringBuilder("\"");
        for (int index = 0; index < text.length(); index++) {
            char character = text.charAt(index);
            switch (character) {
                case '"' -> result.append("\\\"");
                case '\\' -> result.append("\\\\");
                case '\b' -> result.append("\\b");
                case '\f' -> result.append("\\f");
                case '\n' -> result.append("\\n");
                case '\r' -> result.append("\\r");
                case '\t' -> result.append("\\t");
                default -> {
                    if (character < 0x20) {
                        result.append(
                            String.format(
                                Locale.ROOT,
                                "\\u%04x",
                                (int) character
                            )
                        );
                    } else {
                        result.append(character);
                    }
                }
            }
        }
        return result.append('"').toString();
    }

    public static String toJson(Database database) {
        StringBuilder output = new StringBuilder();
        output.append("{\n");
        output.append("  \"version\": ").append(database.version()).append(",\n");
        output.append("  \"flags\": ").append(database.flags()).append(",\n");
        output.append("  \"generation\": ")
            .append(database.generation())
            .append(",\n");
        output.append("  \"sha256\": ")
            .append(jsonEscape(database.sha256()))
            .append(",\n");
        output.append("  \"tables\": [\n");
        for (int tableIndex = 0;
             tableIndex < database.tables().size();
             tableIndex++) {
            Table table = database.tables().get(tableIndex);
            output.append("    {\n");
            output.append("      \"name\": ")
                .append(jsonEscape(table.name()))
                .append(",\n");
            output.append("      \"row_count\": ")
                .append(table.rows().size())
                .append(",\n");
            output.append("      \"columns\": [\n");
            for (int columnIndex = 0;
                 columnIndex < table.columns().size();
                 columnIndex++) {
                Column column = table.columns().get(columnIndex);
                output.append("        {\"name\": ")
                    .append(jsonEscape(column.name()))
                    .append(", \"type\": ")
                    .append(jsonEscape(column.type().name().toLowerCase(Locale.ROOT)))
                    .append(", \"nullable\": ")
                    .append(column.nullable())
                    .append(", \"primary_key\": ")
                    .append(column.primaryKey())
                    .append(", \"unique\": ")
                    .append(column.unique())
                    .append("}");
                if (columnIndex + 1 < table.columns().size()) {
                    output.append(',');
                }
                output.append('\n');
            }
            output.append("      ]\n");
            output.append("    }");
            if (tableIndex + 1 < database.tables().size()) {
                output.append(',');
            }
            output.append('\n');
        }
        return output.append("  ]\n}\n").toString();
    }

    private static void printHuman(Path path, Database database, int rowLimit) {
        System.out.printf(
            Locale.ROOT,
            "%s: version=%d generation=%d tables=%d rows=%d sha256=%s%n",
            path,
            database.version(),
            database.generation(),
            database.tables().size(),
            database.rowCount(),
            database.sha256()
        );
        for (Table table : database.tables()) {
            System.out.printf(
                Locale.ROOT,
                "  %s: %d columns, %d rows%n",
                table.name(),
                table.columns().size(),
                table.rows().size()
            );
            System.out.print("    ");
            for (int index = 0; index < table.columns().size(); index++) {
                if (index != 0) {
                    System.out.print(" | ");
                }
                Column column = table.columns().get(index);
                System.out.print(column.name() + ":" + column.type());
            }
            System.out.println();
            int shown = Math.min(rowLimit, table.rows().size());
            for (int rowIndex = 0; rowIndex < shown; rowIndex++) {
                Row row = table.rows().get(rowIndex);
                System.out.print("    ");
                for (int index = 0; index < row.values().size(); index++) {
                    if (index != 0) {
                        System.out.print(" | ");
                    }
                    System.out.print(row.values().get(index).display());
                }
                System.out.println();
            }
            if (shown < table.rows().size()) {
                System.out.printf(
                    Locale.ROOT,
                    "    ... %d rows omitted%n",
                    table.rows().size() - shown
                );
            }
        }
    }

    private static void usage() {
        System.err.println(
            "Usage: QueryForgeInspector [--json] [--rows N] DATABASE..."
        );
    }

    public static void main(String[] arguments) {
        boolean json = false;
        int rowLimit = 5;
        List<Path> paths = new ArrayList<>();
        for (int index = 0; index < arguments.length; index++) {
            String argument = arguments[index];
            if (argument.equals("--json")) {
                json = true;
            } else if (argument.equals("--rows")) {
                if (++index >= arguments.length) {
                    usage();
                    System.exit(2);
                }
                try {
                    rowLimit = Integer.parseInt(arguments[index]);
                    if (rowLimit < 0 || rowLimit > 10_000) {
                        throw new NumberFormatException();
                    }
                } catch (NumberFormatException error) {
                    System.err.println("--rows must be between 0 and 10000");
                    System.exit(2);
                }
            } else if (argument.startsWith("-")) {
                usage();
                System.exit(2);
            } else {
                paths.add(Path.of(argument));
            }
        }
        if (paths.isEmpty() || (json && paths.size() != 1)) {
            usage();
            System.exit(2);
        }
        try {
            for (Path path : paths) {
                Database database = read(path);
                if (json) {
                    System.out.print(toJson(database));
                } else {
                    printHuman(path, database, rowLimit);
                }
            }
        } catch (IOException | FormatException error) {
            System.err.println("QueryForgeInspector: " + error.getMessage());
            System.exit(1);
        }
    }
}
