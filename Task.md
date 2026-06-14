# Socket Codec
**Socket Codec** is a consistent, low-latency real-time communication application. Its core insight is that the socket, or network layer, should be tightly integrated with the codec. Instead of treating the codec as a black box or handling encoded frames as bursty data flows, we should control the codec at the network-packet level.

The key ideas include:

1. **Packet-level error concealment**  
   Traditional codec-level error concealment often handles packet loss by sending the next frame as an I-frame, which significantly increases frame size and can cause latency spikes. Instead, we can retransmit only the lost region as intra-coded blocks, reducing recovery cost and avoiding sudden delay spikes.

2. **Packet-level rate control**  
   Current codecs typically update `target_bitrate` only after the current frame has finished encoding. But what if the network condition drops during the encoding process? We can split the encoding process into smaller stages and insert idle time so that encoding is spread evenly across the frame interval. This allows us to adjust the target bitrate immediately within the current frame, rather than waiting for the next one.

## Some key rules
1. Before write any task, first generate a spec, including plan.md, todo.md(with checkbox), and need to wait for approval.
2. After finish the plan doc, first write tests to regular every function's input and expected output.
3. For every sub function, git commit when you think it finish a specific function (usually under 200 lines change), do not commit a super-huge git commit.
4. We are an experimental project, don't consider corner case, your goal is to finish the basic function first.

## Tasks
1. Design the whole RTC pipeline plan doc, current code has a very basic pipeline to transmit the encoded frame through UDP. But the real pipeline should contain more abilities.
   1. transmission should have congestion control component, using Google Congestion Control algorithm. You can reference WebRTC as an example, but don't write such many code.
   2. You also should keep the potential that I can involve more other congestion control algorithms (using interface or father-child class)
   3. mahimahi cannot be used on mac, implement a simple replace of it, because we don't need to test across devices, only need to mock a bandwidth control or packet loss, rtt control. If it's not easy to work on system level (because we directly call socket send, which is not easy to hack), you can add code into our project to control this.
   4. Current codec we first use x264 to implement the MVP version.
   5. Verify if slice number can be changed during encoding (this frame using 10x10, next frame using 2x2 because the target bitrate changed)
   6. Because we want packet-level rate control, the encoding process cannot be finished at one time, we should split the tasks into sub-level, and add idle time between them, based on slice number (such as 10x10, then overall have 100 slices, for one frame interval (33ms), slice0 encode at 0, slice1 at 0.33, slice2 at 0.66,...)
2. 