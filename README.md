# octoping

A simplistic implementation of RTT measurements via ping/response over UDP. The client will send series of
UDP packets to the specified destination. The server echoes these packets. The packets
up to three 64 bit integers:

* packet number
* time sent by client
* time received and echoed by server

For each packet, the client will add a line to a csv file (default: test.csv), with 4 columns:

* packet number
* time sent by client
* time received and echoed by server
* time echo received by client.

If a packet is lost, the client will add a line to the CSV file, in which the last two columns will be zero.

Times are in microseconds.

