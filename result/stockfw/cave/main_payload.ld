/* OpenGrand slot payloads of the power family (ota_main): the power port layout (power_payload.ld) plus every stock binding file
 * (SysEx front, BT port, session core).  A build references only the symbols of the ports it links in;
 * the others are unreferenced absolute assignments and change no byte of the output.
 * Valid for stock V2.19 (CAx9_P06.SYS) only.                                                    */
INCLUDE power_payload.ld
INCLUDE stock_sysex.ld
INCLUDE stock_bt.ld
INCLUDE stock_session.ld
INCLUDE stock_relay.ld
INCLUDE stock_rec.ld
INCLUDE stock_chan.ld
INCLUDE stock_xgate.ld
INCLUDE stock_fread.ld
