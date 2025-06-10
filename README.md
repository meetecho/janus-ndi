Janus NDI Plugin
================

This is an implementation of a Janus NDI plugin, developed by [Meetecho](http://www.meetecho.com). Its main purpose is receiving streams via WebRTC, and translating them to NDI senders locally. It's the open source version of a plugin that, at the time, was originally used by [Broadcast Bridge](https://broadcastbridge.app/) to help with the recording of [CommCon Virtual 2021](https://2021.commcon.xyz/) remote presentations.

A [Janode](https://github.com/meetecho/janode/) module is available as well, to control the plugin programmatically via Node.js/JanaScript.

The plugin supports:

* NDI sender with a test pattern and a static name
* Creating one-shot or reusable NDI senders for WebRTC users
* Placeholder images for reusable NDI senders (e.g., when no PeerConnection is feeding them)
* Closing images for one-shot NDI senders (e.g., when the PeerConnection goes away)
* Decode Opus and VP8/VP9/H.264/AV1 (depending on FFmpeg installation) to raw NDI
* Resizing video after decode (with or without keeping the aspect ratio)
* Stereo audio
* Tally events

At the time of writing, the plugin does _NOT_ support:

* Watermarking (partially supported in a private development branch)
* NDI-HX
* Advanced NDI 5 and 6 features (this plugin was implemented when only NDI 4 was available)

To learn more about the plugin, you can refer to [this blog post](https://www.meetecho.com/blog/webrtc-ndi/) and [this other blog post](https://www.meetecho.com/blog/webrtc-ndi-part-2/), which explain more in detail how it should be used within the context of Janus-based WebRTC conversations.

## Dependencies

The main dependencies are Janus, of course, and the NDI SDK, which needs to be installed as follows:

* headers in /usr/include/NDI
* shared objects to /usr/lib (or /usr/lib64, if it's a 64-bit installation)

To install the plugin itself, you'll also need to satisfy the following dev dependencies:

* [GLib](http://library.gnome.org/devel/glib/)
* [Jansson](http://www.digip.org/jansson/)
* [libcurl](https://curl.haxx.se/libcurl/)
* [ffmpeg-dev](http://ffmpeg.org/)
* [libopus](http://opus-codec.org/)

Support for decoding Opus, VP8, VP9, H.264 and AV1 should be available in the FFmpeg installation, or attempting to decode those codecs will fail.

## Compiling

Set the `JANUSP` env variable to configure where Janus is installed, and then issue `make` to compile the plugin, e.g.:

	JANUSP=/opt/janus make

The plugin will automatically detect whether it's building against Janus `1.x` or `0.x`. Notice that, even when building for `1.x`, this plugin doesn't support multistream: this means that, at the time of writing, each PeerConnection can only be associated with one NDI sender, and each NDI sender can only contain one audio and/or video feed.

## Installing

Set the `JANUSP` env variable to configure where Janus is installed, and then issue `make install` to install the plugin, e.g.:

	JANUSP=/opt/janus make install

This will also install a template configuration file for the plugin (currently limited to a couple of settings).

## Testing

To test the plugin in a local setup, you can use the `ndi.html` web page in the `demo` folder. It will work as any other Janus demo, so please refer to the [related instructions](https://janus.conf.meetecho.com/docs/deploy) in the Janus documentation for info on how to deploy this. You can also use the `start_test_pattern` request to test the plugin without the need to establish a PeerConnection: check the `API` section below for more information.

When using the demo, opening the web page will prompt you for a display name. Once you do that, a `sendonly` PeerConnection will be established with the plugin, and the plugin will create an NDI sender with that display name for you locally. This NDI feed should then become visible to NDI compatible applications (e.g., OBS if you have the NDI plugin installed). Tally events will be displayed in the web page when the related NDI feed is consumed.

Notice that this is just a local demo to showcase the plugin from a functional perspective. In regular scenarios, the Janus instance serving users will not be in the same network as the applications dealing with NDI feeds. Please refer to the blog posts mentioned at the beginning of this page for more information on the type of orchestration you'll need to perform.

A [Janode](https://github.com/meetecho/janode/) module is also available as well, to control the plugin programmatically via Node.js/JanaScript. No example is available as of yet, but if you're familiar with Janode it should be trivial to use. You can learn more [here](janode/README.md).

# API

The `translate` request must be used to setup the PeerConnection and associate it with an NDI source: it expects a `name` property to be used by the NDI sender; optional arguments are `bitrate` (to send a bitrate cap via REMB) and `width`/`height` (to force scaling to a static resolution; if missing, the original resolution in the WebRTC stream is used). The following code comes from the sample demo page:

	ndi.createOffer(
		{
			media: { audio: true, video: true },
			success: function(jsep) {
				Janus.debug("Got SDP!", jsep);
				// Send a request to the plugin
				var translate = {
					request: "translate",
					name: "my-test"
				}
				ndi.send({ message: translate, jsep: jsep });
			},
			error: function(error) {
				Janus.error("WebRTC error:", error);
				bootbox.alert("WebRTC error... " + error.message);
			}
		});

This will create a new NDI source named `my-test` available with the provided audio/video streams. The `hangup` request can be used to tear down the PeerConnection instead: for one-shot NDI senders, this will release the NDI sender as well. A `configure` request can be used to try and tweak a WebRTC stream: `bitrate` will send a bitrate cap via REMB, `keyframe: true` will trigger a PLI. Notice that REMB will be ignored if the NDI plugin is receiving a WebRTC stream from another Janus instance, rather than a browser.

A test pattern can be sent via NDI by using a `start_test_pattern` request, and stopped via `stop_test_pattern`. The test pattern is a static image sent at 30fps via NDI, and so can be used to verify whether or not recipients can obtain NDI streams originated by the plugin. Only a single test pattern can be started at a time, since it has a hardcoded `janus-ndi-test` name. Both `start_test_pattern` and `stop_test_pattern` are synchronous requests, and can be invoked via Admin API as well, which means they can be triggered by, e.g., curl one-liners:

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "start_test_pattern" } }' http://localhost:7088/admin

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "stop_test_pattern" } }' http://localhost:7088/admin

For a more comprehensive documentation, including info on how to pre-create senders and have placeholder images be displayed when a WebRTC connection is not feeding them, please refer to the [API](docs/API.md).
