# Gateway CAN test peer

`can_peer.py` implements a module peer for the [CAN FD v2 prototype contract](../firmware/docs/can-fd-v2.md). Python 3.10+ is sufficient. It does not implement Cyphal or the Cockpit Pico simulator protocol, and it does not flash a module.

Print the exact reference frames on any host:

```sh
python3 tools/can_peer.py --vectors
python3 -m unittest discover -s tools -p 'test_*.py'
```

For a physical test, use a Linux SocketCAN adapter supporting CAN FD+BRS. Configure its interface for **500,000 bit/s arbitration, 2,000,000 bit/s data and FD enabled**. The gateway uses a shared ground and one linear bus with exactly two 120 Ω endpoint terminators. Consult the PCB wiring guide before attaching an adapter; a CAN adapter must not inject a second chain supply. This script never changes the host interface configuration.

```sh
python3 tools/can_peer.py --interface can0
```

Open the gateway CDC console with DTR asserted, send `inventory`, then `register 00112233445566778899aabbccddeeff` and `apply`. The peer obtains a node/lease from the gateway and sends neutral inputs every 10 ms. Registration deliberately reconnects gateway USB; reopen CDC if its port name changes. To exercise nonzero inputs, stop the peer and restart it with the same identity:

```sh
python3 tools/can_peer.py --interface can0 --buttons-mask 0x5 --axis-values 0x1234,0xffff
```

Use `--hats 4 --hat-values 0,7,8,8` for packed hats, and `--show-acks` to print
reports acknowledged only after successful gateway USB delivery. Maximum profiles
support `--buttons 128 --hats 4 --axes 8`.

The new random module boot session revokes its old lease. Stopping the process should restore neutral HID values after the gateway's 250 ms input timeout; the registry slot and USB layout remain present. Restart with `--direct-usb` to test direct USB input ownership: the peer sends ownership heartbeats and routed HID remains neutral. Each additional peer needs a unique `--identity`. Limit live tests to the board's documented total power budget, independent of the firmware's ten-module capacity.

Temporary transmit backpressure pauses input traffic for 500 ms and leaves the
peer running. It resumes with an assignment and preserves the input sequence
when the gateway returns the same lease.

Offline conformance checks compare the Python frames against stored firmware
vectors, validate assignments, reject reserved identities and simulate transmit
queue recovery. Set `OSG_GATEWAY_TEST` to the built native `osg_gateway_test`
executable to also compare freshly encoded C frames; CI enables that check.
Live SocketCAN and assembled gateway tests have not run in this workspace; no
physical CAN adapter is connected here.
