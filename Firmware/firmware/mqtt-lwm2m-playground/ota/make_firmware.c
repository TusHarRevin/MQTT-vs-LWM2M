/* make_firmware <out.bin> <version> <size-bytes>
 * Writes a SIMULATED firmware image (see fw_image.h). Run by the build. */

#include <stdio.h>
#include <stdlib.h>

#include "fw_image.h"

int main(int argc, char **argv)
{
    uint32_t size;
    uint8_t *buf;
    FILE *f;

    if (argc != 4) {
        fprintf(stderr, "usage: make_firmware <out.bin> <version> <size-bytes>\n");
        return 2;
    }
    size = (uint32_t)strtoul(argv[3], NULL, 10);
    buf = malloc(size ? size : 1);
    if (buf == NULL) {
        return 1;
    }
    fw_image_fill(buf, size, argv[2]);
    f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(buf, 1, size, f) != size) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    fclose(f);
    free(buf);
    printf("wrote %s (version %s, %u bytes, SIMULATED)\n", argv[1], argv[2], (unsigned)size);
    return 0;
}
