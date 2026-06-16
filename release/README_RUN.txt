Tarkov Voice Overlay release folder

1. Start a local coordinator:
   start_public_server.cmd

2. To run the client:
   start_client.cmd

3. For a public server, set the coordinator address before starting the client:
   set TVO_COORDINATOR_URL=wss://your-host.example/ws

4. To use a private room password, set it locally before starting:
   set TVO_ROOM_PASSWORD=change-this-locally

5. The client opens a small setup window. Enter your nickname, choose an icon,
   set microphone sensitivity and microphone volume, then press Start.

6. To stop the client:
   stop_client.cmd
   You can also click the small X on the overlay.

7. To stop local public server processes:
   stop_public_server.cmd

The Test button records your microphone for 2 seconds and immediately plays the
recording back so you can hear the level. It also saves the last raw recording to
logs\mic-test.wav for troubleshooting.
