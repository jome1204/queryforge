#define _CRT_SECURE_NO_WARNINGS

/*
 * Small dependency-free QueryForge image probe.
 *
 * It is intentionally not a replacement for the C++ database implementation.
 * The program is useful in shell scripts that only need header/checksum facts.
 */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QF_MAX_FILE_BYTES (256u * 1024u * 1024u)
#define QF_HEADER_BYTES 32u
struct qf_header {
  uint32_t version;
  uint32_t table_count;
  uint64_t row_count;
  uint32_t page_size;
};

static uint32_t read_u32(const unsigned char *data) {
  return ((uint32_t)data[0]) |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static uint64_t read_u64(const unsigned char *data) {
  return ((uint64_t)read_u32(data)) |
         ((uint64_t)read_u32(data + 4) << 32);
}

static uint32_t crc32_update(
    uint32_t checksum,
    const unsigned char *data,
    size_t size) {
  size_t index;
  checksum = ~checksum;
  for (index = 0; index < size; ++index) {
    unsigned bit;
    checksum ^= data[index];
    for (bit = 0; bit < 8; ++bit) {
      uint32_t mask = (uint32_t)-(int32_t)(checksum & 1u);
      checksum = (checksum >> 1) ^ (0xedb88320u & mask);
    }
  }
  return ~checksum;
}

static int parse_header(
    const unsigned char *data,
    size_t size,
    struct qf_header *header,
    char *message,
    size_t message_size) {
  static const unsigned char signature[8] = {
      'Q', 'F', 'E', 'N', 'G', '1', 0, 0};
  uint32_t expected_checksum;
  uint32_t actual_checksum;
  if (size < QF_HEADER_BYTES) {
    snprintf(message, message_size, "file is shorter than the header");
    return 0;
  }
  if (memcmp(data, signature, sizeof(signature)) != 0) {
    snprintf(message, message_size, "database signature does not match");
    return 0;
  }
  header->version = read_u32(data + 8);
  header->table_count = read_u32(data + 12);
  header->row_count = read_u64(data + 16);
  header->page_size = read_u32(data + 24);
  if (header->version != 1u) {
    snprintf(
        message,
        message_size,
        "unsupported database version %" PRIu32,
        header->version);
    return 0;
  }
  if (header->table_count > 4096u) {
    snprintf(message, message_size, "table count exceeds resource limit");
    return 0;
  }
  if (header->page_size != 4096u) {
    snprintf(message, message_size, "unsupported database page size");
    return 0;
  }
  expected_checksum = read_u32(data + 28);
  actual_checksum = crc32_update(0, data + 32, size - 32);
  if (expected_checksum != actual_checksum) {
    snprintf(
        message,
        message_size,
        "checksum mismatch: %08" PRIx32 " != %08" PRIx32,
        expected_checksum,
        actual_checksum);
    return 0;
  }
  return 1;
}

static unsigned char *read_file(
    const char *path,
    size_t *size,
    char *message,
    size_t message_size) {
  FILE *input;
  long length;
  unsigned char *data;
  size_t received;
  *size = 0;
  input = fopen(path, "rb");
  if (input == NULL) {
    snprintf(message, message_size, "cannot open: %s", strerror(errno));
    return NULL;
  }
  if (fseek(input, 0, SEEK_END) != 0) {
    snprintf(message, message_size, "cannot seek: %s", strerror(errno));
    fclose(input);
    return NULL;
  }
  length = ftell(input);
  if (length < 0) {
    snprintf(message, message_size, "cannot determine file length");
    fclose(input);
    return NULL;
  }
  if ((unsigned long)length > QF_MAX_FILE_BYTES) {
    snprintf(message, message_size, "file exceeds resource limit");
    fclose(input);
    return NULL;
  }
  if (fseek(input, 0, SEEK_SET) != 0) {
    snprintf(message, message_size, "cannot rewind: %s", strerror(errno));
    fclose(input);
    return NULL;
  }
  data = (unsigned char *)malloc((size_t)length == 0 ? 1u : (size_t)length);
  if (data == NULL) {
    snprintf(message, message_size, "memory allocation failed");
    fclose(input);
    return NULL;
  }
  received = fread(data, 1, (size_t)length, input);
  if (received != (size_t)length) {
    snprintf(message, message_size, "short read: %s", strerror(errno));
    free(data);
    fclose(input);
    return NULL;
  }
  if (fclose(input) != 0) {
    snprintf(message, message_size, "close failed: %s", strerror(errno));
    free(data);
    return NULL;
  }
  *size = received;
  return data;
}

static void print_text(
    const char *path,
    size_t size,
    const struct qf_header *header) {
  printf("%s\n", path);
  printf("  bytes: %zu\n", size);
  printf("  version: %" PRIu32 "\n", header->version);
  printf("  tables: %" PRIu32 "\n", header->table_count);
  printf("  rows: %" PRIu64 "\n", header->row_count);
  printf("  page size: %" PRIu32 "\n", header->page_size);
  printf("  checksum: valid\n");
}

static void print_json(
    const char *path,
    size_t size,
    const struct qf_header *header) {
  const unsigned char *cursor = (const unsigned char *)path;
  printf("{\"path\":\"");
  while (*cursor != 0) {
    unsigned char character = *cursor++;
    if (character == '"' || character == '\\') {
      putchar('\\');
      putchar(character);
    } else if (character < 0x20) {
      printf("\\u%04x", (unsigned)character);
    } else {
      putchar(character);
    }
  }
  printf(
      "\",\"bytes\":%zu,\"version\":%" PRIu32
      ",\"tables\":%" PRIu32 ",\"rows\":%" PRIu64
      ",\"page_size\":%" PRIu32 ",\"checksum\":true}\n",
      size,
      header->version,
      header->table_count,
      header->row_count,
      header->page_size);
}

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [--json] DATABASE...\n", program);
}

int main(int argc, char **argv) {
  int json = 0;
  int index = 1;
  int failures = 0;
  if (index < argc && strcmp(argv[index], "--json") == 0) {
    json = 1;
    ++index;
  }
  if (index >= argc) {
    usage(argv[0]);
    return 2;
  }
  for (; index < argc; ++index) {
    const char *path = argv[index];
    unsigned char *data;
    size_t size;
    struct qf_header header;
    char message[256];
    if (path[0] == '-') {
      usage(argv[0]);
      return 2;
    }
    data = read_file(path, &size, message, sizeof(message));
    if (data == NULL) {
      fprintf(stderr, "%s: %s\n", path, message);
      failures = 1;
      continue;
    }
    if (!parse_header(data, size, &header, message, sizeof(message))) {
      fprintf(stderr, "%s: %s\n", path, message);
      failures = 1;
      free(data);
      continue;
    }
    if (json) {
      print_json(path, size, &header);
    } else {
      print_text(path, size, &header);
    }
    free(data);
  }
  return failures;
}
