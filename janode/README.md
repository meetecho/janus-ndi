# Janode module for Janus NDI plugin

[Janode](https://github.com/meetecho/janode/) is a Node.js, browser compatible, adapter for the Janus WebRTC server. It provides an extensible mechanism for adding support to more plugins than those available out of the box. This is an implementation of a Janode module for the Janus NDI plugin.

You can refer to the [Janode documentation](https://meetecho.github.io/janode/) for more info on how to use Janode itself. For those familiar with Janode, using the NDI plugin should be fairly trivial.

## Example of usage

```js
import Janode from 'janode';
const { Logger } = Janode;
import JanusNdiPlugin from 'janode-ndi';
 
const connection = await Janode.connect({
  is_admin: false,
  address: {
    url: 'ws://127.0.0.1:8188/',
    apisecret: 'secret'
  }
});
const session = await connection.create();

// Attach to the NDI plugin
const ndiHandle = await session.attach(JanusNdiPlugin)

// Subscribe to tally events
ndiHandle.on(JanusNdiPlugin.EVENT.JANUS_NDI_TALLY, evtdata => Logger.info('tally', evtdata));

// Start the NDI test pattern
await ndiHandle.startTestPattern();

// Publish to the NDI plugin
const { jsep: answer } = await ndiHandle.translate({ name: 'My NDI Video', jsep: offer });

// Refer to the code for more requests
```
