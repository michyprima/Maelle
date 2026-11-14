// The console's network calls that the network code makes, built with wut's headers as it is; the guest has no network,
// so Wi-Fi is never connected, see runtime.cpp
#include <arpa/inet.h>
#include <nn/ac/ac_c.h>

NNResult ACInitialize() { return { -1 }; }
void ACFinalize() {}
NNResult ACConnect() { return { -1 }; }
NNResult ACIsApplicationConnected(BOOL *connected) { *connected = FALSE; return { -1 }; }
uint32_t htonl(uint32_t value) { return value; } // Big-endian, so network order already
