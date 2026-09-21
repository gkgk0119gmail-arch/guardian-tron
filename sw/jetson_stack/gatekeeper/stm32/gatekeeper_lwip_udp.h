#ifndef GATEKEEPER_LWIP_UDP_H
#define GATEKEEPER_LWIP_UDP_H

/* Call this once, AFTER MX_LWIP_Init() has run (so the netif/IP stack is up)
 * and after your Ethernet link is confirmed. It binds a UDP PCB on port
 * 5005 (matches jetson/ecu/udp_link.py DEFAULT_PORT / docs/PROTOCOL.md) and
 * registers the receive callback that runs the exact same gatekeeper logic
 * validated on the Jetson via gatekeeper/build/gatekeeper_udp. */
void gatekeeper_udp_init(void);

#endif /* GATEKEEPER_LWIP_UDP_H */
