/* Synthetic static TLS control for the stock-loader stack boundary probe.
 * Compile with -DTLS_BYTES=<positive size> and preload at process startup.
 */
#ifndef TLS_BYTES
#error TLS_BYTES is required
#endif
#ifndef TLS_ALIGN
#define TLS_ALIGN 4
#endif

static __thread unsigned char reserve[TLS_BYTES]
    __attribute__((tls_model("initial-exec"), aligned(TLS_ALIGN), used));

int tls_probe(void) { return reserve[0]; }
