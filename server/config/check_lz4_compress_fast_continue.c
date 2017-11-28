#include <lz4.h>

int main()
{
    if (0) {
        LZ4_stream_t *stream = NULL;
        const char *in_buf = NULL;
        char *compressed_buf = NULL;
        int in_size = 0;
        int bound_size = 0;
        LZ4_compress_fast_continue(stream,
                                   in_buf, compressed_buf,
                                   in_size, bound_size, 1);
    }
    return 0;
}
