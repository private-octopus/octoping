# octoping

A simplistic implementation of RTT measurements via ping/response over UDP. The client will send series of
UDP packets to the specified destination. The server echoes these packets. The packets
contain up to three 64 bit integers:

* packet number
* time sent by client
* time received and echoed by server

For each packet, the client will add a line to a csv file (default: stdout), with 8 columns:

* packet number
* time sent by client
* time received and echoed by server, per server's clock
* time echo received by client
* rtt
* estimated one way delay from client to server
* estimated one way delay from server to client
* phase estimate (delta between client and server clock)

All times and delays are expressed in microseconds. The sent, received and
echo times are expressed as delai since the first packet was sent.

Computing the one way delays requires estimating the phase difference
between the client clock and the server clock, which is done as follow:

* for the first packet received, assume that the phase is the difference
  between the reported server time and the middle of time sent and time
  received.
* for the next packets, check the rtt. If it close to rtt_min, compute
  a phase sample as the difference between the reported server time
  and the middle of time sent and time received. Update the phase
  using an exponential averaging algorithm.
* if the current phase value leads to dengative up or down delays,
  reset the phase as the difference
  between the reported server time and the middle of time sent and time
  received.

If a packet is lost, the client will add a line to the CSV file, in which only the
packet number and time sent are not zero.

