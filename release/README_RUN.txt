Tarkov Voice Overlay release folder

1. Public coordinator:
   ws://185.244.51.198:8080/ws

2. To run the client:
   start_client.cmd

3. To use a private room password, set it locally before starting:
   set TVO_ROOM_PASSWORD=change-this-locally

4. The client opens a small setup window. Enter your nickname, choose an icon,
   set microphone sensitivity and microphone volume, then press Start.

5. To stop the client:
   stop_client.cmd
   You can also click the small X on the overlay.

6. To run a local coordinator instead, start it and override the client address:
   start_public_server.cmd
   set TVO_COORDINATOR_URL=ws://127.0.0.1:8080/ws

The Test button records your microphone for 2 seconds and immediately plays the
recording back so you can hear the level. It also saves the last raw recording to
logs\mic-test.wav for troubleshooting.
