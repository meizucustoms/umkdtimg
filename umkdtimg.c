#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>

#include <sys/file.h>
#include <sys/stat.h>

// See https://source.android.com/devices/architecture/dto/partitions
#define DT_TABLE_MAGIC 0xd7b7ab1e

struct dt_table_header {
    uint32_t magic;             // DT_TABLE_MAGIC
    uint32_t total_size;        // includes dt_table_header + all dt_table_entry
                                // and all dtb/dtbo
    uint32_t header_size;       // sizeof(dt_table_header)

    uint32_t dt_entry_size;     // sizeof(dt_table_entry)
    uint32_t dt_entry_count;    // number of dt_table_entry
    uint32_t dt_entries_offset; // offset to the first dt_table_entry
                                // from head of dt_table_header

    uint32_t page_size;         // flash page size we assume
    uint32_t version;       // DTBO image version, the current version is 0.
                            // The version will be incremented when the
                            // dt_table_header struct is updated.
};

struct dt_table_entry {
    uint32_t dt_size;
    uint32_t dt_offset;         // offset from head of dt_table_header

    uint32_t id;                // optional, must be zero if unused
    uint32_t rev;               // optional, must be zero if unused
    uint32_t custom[4];         // optional, must be zero if unused
};

// read() wrapper for easier repeated usage
int read_file(char *filename, void *buf, off_t offset, size_t count) {
    int ret = 0, fd = 0;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "read_file: failed to open %s\n", filename);
        return fd;
    }

    ret = lseek(fd, offset, SEEK_SET);
    if (ret < 0) {
        fprintf(stderr, "read_file: failed to jump to pos %ld (%s)\n", offset, filename);
        free(buf);
        close(fd);
        return ret;
    }

    ret = read(fd, buf, count);
    if (ret < 0) {
        fprintf(stderr, "read_file: failed to read %s %d\n", filename, ret);
        free(buf);
        close(fd);
        return ret;
    }

    close(fd);
    return 0;
}

uint8_t *dtbo_file = NULL;
size_t dtbo_sz = 0;

void show_help() {
    fprintf(stderr, "Usage: ./umkdtimg [-i|--input] dtbo.img [-o|--output] out_dir\n");
}

int main(int argc, char **argv) {
    char *filename = NULL;
    char *out_dir = "./";

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input")) {
            if ((i + 1) == argc) {
                show_help();
                fprintf(stderr, "Please specify filename with -i argument\n");
                return EINVAL;
            }
            i++;

            filename = argv[i];
        } else if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
            if ((i + 1) == argc) {
                show_help();
                fprintf(stderr, "Please specify directory with -o argument\n");
                return EINVAL;
            }
            i++;

            filename = argv[i];
        }
    }

    if (filename == NULL) {
        show_help();
        fprintf(stderr, "DTBO filename is not defined\n");
        return EINVAL;
    }

    struct stat st;
    stat(filename, &st);
    dtbo_sz = st.st_size;
    dtbo_file = malloc(dtbo_sz);

    int ret = read_file(filename, (void **)dtbo_file, 0, dtbo_sz);
    if (ret < 0) {
        fprintf(stderr, "Failed to read %s\n", filename);
        return ret;
    }

    struct dt_table_header *header = (struct dt_table_header *)dtbo_file;

    printf("Magic: 0x%x (%s)\n", htobe32(header->magic), htobe32(header->magic) == DT_TABLE_MAGIC ? "valid" : "invalid");
    printf("Total size: %d bytes\n", htobe32(header->total_size));
    printf("Header size: %d bytes\n", htobe32(header->header_size));
    printf("DT entry size: %d bytes\n", htobe32(header->dt_entry_size));
    printf("DT entries count: %d\n", htobe32(header->dt_entry_count));
    printf("Header -> first DT entry offset: %d bytes\n", htobe32(header->dt_entries_offset));
    printf("Page size: %d bytes\n", htobe32(header->page_size));
    printf("DTBO version: %d\n", htobe32(header->version));

    if (htobe32(header->magic) == DT_TABLE_MAGIC && htobe32(header->dt_entry_count) != 0) {
        printf("\n- Dumping DTBs...\n\n");

        for (int i = 0; i < htobe32(header->dt_entry_count); i++) {
            struct dt_table_entry *entry = malloc(htobe32(header->dt_entry_size));
            int ppos = htobe32(header->dt_entries_offset) +
                        (i * htobe32(header->dt_entry_size));

            ret = read_file(filename, (void **)entry, ppos, 
                                    sizeof(struct dt_table_entry));
            if (ret < 0) {
                fprintf(stderr, "Failed to read DT entry %d\n", i + 1);
                free(dtbo_file);
                return ret;
            }

            size_t entry_offset = htobe32(entry->dt_offset);
            off_t entry_size = htobe32(entry->dt_size);
            uint8_t *entry_contents = malloc(entry_size);

            printf("Found DTB #%d: id: 0x%04x, rev: 0x%04x, custom: [0x%x, 0x%x, 0x%x, 0x%x], size: %d, offset: %d\n", 
                    i + 1, htobe32(entry->id), htobe32(entry->rev), 
                    htobe32(entry->custom[0]), htobe32(entry->custom[1]), 
                    htobe32(entry->custom[2]), htobe32(entry->custom[3]),
                    entry_size, entry_offset);

            ret = read_file(filename, entry_contents, entry_offset, entry_size);
            if (ret < 0) {
                fprintf(stderr, "Failed to read DT entry %d contents\n", i + 1);
                free(entry);
                free(entry_contents);
                free(dtbo_file);
                return ret;
            }

            if (stat(out_dir, &st) == -1) {
                mkdir(out_dir, 0755);
            }

            // Construct filename
            char *realPath = malloc(strlen(out_dir) + 32);
            if (out_dir[strlen(out_dir) - 1] == '/') {
                sprintf(realPath, "%s%02d_0x%04x_0x%04x.dtb", 
                        out_dir, i + 1, htobe32(entry->id), htobe32(entry->rev));
            } else {
                sprintf(realPath, "%s/%02d_0x%04x_0x%04x.dtb", 
                        out_dir, i + 1, htobe32(entry->id), htobe32(entry->rev));
            }

            // Store DTB
            int fd = open(realPath, O_WRONLY | O_CREAT, 0755);
            if (fd < 0) {
                fprintf(stderr, "Failed to create %s\n", realPath);
                free(entry);
                free(entry_contents);
                free(dtbo_file);
                return fd;
            }

            ret = write(fd, entry_contents, entry_size);
            if (ret < 0) {
                fprintf(stderr, "Failed to store DT entry %d contents to file\n", i + 1);
                free(entry);
                free(entry_contents);
                free(dtbo_file);
                return ret;
            }

            printf("Stored DTB #%d: id: %d, rev: %d, custom: [0x%x, 0x%x, 0x%x, 0x%x]\n", 
                    i + 1, htobe32(entry->id), htobe32(entry->rev), 
                    htobe32(entry->custom[0]), htobe32(entry->custom[1]), 
                    htobe32(entry->custom[2]), htobe32(entry->custom[3]));

            close(fd);
            free(entry);
            free(entry_contents);
        }
    }

    free(dtbo_file);
    return 0;
}
