# Janus NDI Plugin API

This document describes how to interact with the Janus NDI Plugin, using the Janus or Admin API. The first section will cover how the plugin works in general: a section containing info on the API itself will follow.

## Converting a WebRTC PeerConnection to an NDI source

In order to generate an NDI source that other applications (e.g., OBS) can consume, the Janus NDI Plugin needs to have access to a live WebRTC stream it can decode and process accordingly: more precisely, the Janus NDI Plugin works under the assumption that there will always be a 1-1 relationship between a specific PeerConnection and the NDI source it will create on its behalf. From a negotiation perspective, the plugin always expects an offer, which means it's up to the application to provide info on the stream to "translate", and how to translate it. A simplified diagram is provided below:

```
+-------------+                      +-------+                         +-----------+                                 +-----+
| Application |                      | Janus |                         | NDIplugin |                                 | OBS |
+-------------+                      +-------+                         +-----------+                                 +-----+
       |                                 |                                   |                                          |
       | message(details, SDP offer)     |                                   |                                          |
       |-------------------------------->|                                   |                                          |
       |                                 | ------------------------\         |                                          |
       |                                 |-| create PeerConnection |         |                                          |
       |                                 | |-----------------------|         |                                          |
       |                                 |                                   |                                          |
       |                                 | message(details, SDP offer)       |                                          |
       |                                 |---------------------------------->|                                          |
       |                                 |                                   | -------------------------\               |
       |                                 |                                   |-| process asynchronously |               |
       |                                 |                                   | |------------------------|               |
       |                                 |                                   |                                          |
       |                                 |                               ack |                                          |
       |                                 |<----------------------------------|                                          |
       |                                 |                                   |                                          |
       |                             ack |                                   |                                          |
       |<--------------------------------|                                   |                                          |
       |                                 |                                   | --------------------------------------\  |
       |                                 |                                   |-| create answer, decoders, NDI sender |  |
       |                                 |                                   | |-------------------------------------|  |
       |                                 |                                   |                                          |
       |                                 |        event(details, SDP answer) |                                          |
       |                                 |<----------------------------------|                                          |
       |                                 |                                   |                                          |
       |      event(details, SDP answer) |                                   |                                          |
       |<--------------------------------|                                   |                                          |
       |                                 |                                   |                                          |
       | SRTP (audio/video)              |                                   |                                          |
       |-------------------------------->|                                   |                                          |
       |                                 |                                   |                                          |
       |                                 | RTP (audio/video)                 |                                          |
       |                                 |---------------------------------->|                                          |
       |                                 |                                   | --------------------------------------\  |
       |                                 |                                   |-| asynchronously decode and translate |  |
       |                                 |                                   | |-------------------------------------|  |
       |                                 |                                   |                                          |
       |                                 |                                   | NDI (audio/video)                        |
       |                                 |                                   |----------------------------------------->|
       |                                 |                                   |                                          |
```

Giving for granted that the application already created a Janus session and attached to the Janus NDI Plugin as usual, the process to create a new NDI source is pretty straightforward:

1. The application negotiates a new PeerConnection with the plugin, sending an SDP offer, and including some plugin-related details (e.g., NDI name to use).
2. The PeerConnection negotiation is handled as usual by Janus.
3. The plugin creates the resources it needs, including the audio/video decoders and the NDI new sender.
4. After an SDP answer is sent back asynchronously and a PeerConnection eventually created, audio and video start flowing via WebRTC.
5. The plugin decodes all audio and video packets, and "translates" them to NDI.
6. Applications interested in the stream (e.g., OBS) can subscribe to the NDI feed and receive it.

Considering the Janus NDI Plugin will be located in a private network (the same as the one the NDI consumers will be in), most of the times there will not be a direct ingestion by browsers to the plugin. A common scenario will be the Janus NDI Plugin actually receiving SDP offers from other servers (e.g., a Janus instance in the cloud implementing a videoconferencing application). How to orchestrate the communication is out of scope to this document: the only relevant piece of information is that the Janus NDI Plugin expects an offer, and so it will be up to the controlling application to make sure this happens as expected (e.g., by triggering a subscription on a remote VideoRoom instance to get an offer to use, and passing the answer generated by the NDI plugin back to the remote VideoRoom).

Once an WebRTC-to-NDI session has been established, there will be a few things that can be done to tweak the behaviour dynamically, as explained in the API section.

## API

As most existing Janus plugin, the NDI plugin uses the `request` attribute to identify the specific request to perform in its custom API. The Janus NDI Plugin supports a few different requests:

* `create`: create a new NDI sender, with a placeholder image (optional);
* `update_img`: change the placeholder image to use for a shared NDI sender;
* `list`: list the existing shared NDI senders;
* `destroy`: destroy a shared NDI sender;
* `translate`: create a new WebRTC-to-NDI session (possibly referring to an existing NDI sender);
* `configure`: perform a tweak on an existing WebRTC-to-NDI session;
* `hangup`: tear down an existing WebRTC-to-NDI session;
* `start_test_pattern`: send a preconfigured static video test pattern via NDI (useful for testing purposes);
* `stop_test_pattern`: stop the static test pattern.

The following subsections will provide more details on each of them.

### create

While an NDI sender can be created on the fly with `translate`, as we'll see later, in some cases it may be helpful to pre-create an NDI sender independently of whether or not there's a WebRTC PeerConnection to translate. This may come in handy when you want an NDI stream to be always available as a placeholder image (e.g., to fill slots in a produced layout), and then dynamically feed it with a specific stream later on. This is what `create` allows you to do.

The only mandatory argument in the `create` request is `name`, which specifies which name the NDI sender will need to use: this is how NDI consumers will identify the streams when listing available sources. You can also specify an `placeholder` to use as a placeholder (as a `file://` or `http://`/`https://` url): if you don't specify an `image`, the default test pattern will be used instead. The optional `width` and `height` attributes can be used to force the placeholder image to be resized to a specific resolution, and `keep_ratio` dictates whether aspect ratio should be preserved when resizing: when preserving the aspect ratio, horizontal or vertical black stripes may be added to the image to fit the target resolution.

The format of the `create` request is the following:

	{
		"request": "create",
		"name": "<unique name to use for the NDI sender; mandatory>",
		"placeholder": "<local or web path to a placeholder image to use when no active stream is feeding the sender; optional>",
		"width": <width to forcibly scale the placeholder image to; optional>,
		"height": <height to forcibly scale the placeholder image to; optional>,
		"keep_ratio": <whether the aspect ratio should be kept when scaling; optional, true by default>
	}

It's a synchronous request, which means it can also be triggered via Admin API, which makes it easy to "fire" via, e.g., a curl one-liner:

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "create", "name": "lorenzo" } }' http://localhost:7088/admin

A successful processing of the request will look like this:

	{
		"ndi": "success"
	}

### update_img

An NDI sender created with `create` will send a specific image any time a PeerConnection is not actively feeding it with live video. Normally, an image is only provided when `create` is called, but in case the image needs to be changed dynamically (e.g., to re-use the same NDI session for different people, or to provide context-specific images), then `update_img` can be used for the purpose.

The only mandatory arguments in the `update_ing` request are `name`, which specifies the NDI sender to update, and `image`, to provide the new image to use as a placeholder (as a `file://` or `http://`/`https://` url). In case updating the image fails, the previous one will remain active in the sender. As in `create`, the `width`, `height` and `keep_ratio` attributes can be provided as well.

The format of the `update_img` request is the following:

	{
		"request": "update_img",
		"name": "<unique name of the NDI sender to destroy; mandatory>",
		"image": "<local or web path to a placeholder image to use when no active stream is feeding the sender; mandatory>",
		"width": <width to forcibly scale the placeholder image to; optional>,
		"height": <height to forcibly scale the placeholder image to; optional>,
		"keep_ratio": <whether the aspect ratio should be kept when scaling; optional, true by default>
	}

It's a synchronous request, which means it can also be triggered via Admin API, which makes it easy to "fire" via, e.g., a curl one-liner:

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "update_img", "name": "lorenzo", "placeholder": "file:///home/lminiero/Downloads/lminiero-square.png" } }' http://localhost:7088/admin

A successful processing of the request will look like this:

	{
		"ndi": "success"
	}

### list

You can list the existing shared NDI senders with the `list` request.

The format of the `list` request is the following:

	{
		"request": "list"
	}

It's a synchronous request, which means it can also be triggered via Admin API, which makes it easy to "fire" via, e.g., a curl one-liner:

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "list" } }' http://localhost:7088/admin

A successful processing of the request will look like this:

	{
		"ndi": "success",
		"list": [
			{
				"name": "<name of this shared NDI sender>",
				"busy": <true|false, whether the sender is in use>,
				"placeholder": <true|false, whether the sender has a placeholder image>,
				"last_updated": <monotonic time of then the sender was last fed with live data from a PeerConnection>
			},
			... other senders ...
		]
	}

### destroy

An NDI sender created with `create` survives PeerConnections being closed. This means that an ad-hoc request is needed to get rid of it, when it's no longer needed, which is what `destroy` is for. Notice that an NDI sender can only be destroyed if it's not actually in use: if a WebRTC PeerConnection is currently feeding it, `destroy` will return an error: you'll need the PeerConnection to be closed first.

The only mandatory argument in the `create` request is `name`, which specifies the name of the NDI sender to destroy.

The format of the `destroy` request is the following:

	{
		"request": "destroy",
		"name": "<unique name of the NDI sender to destroy; mandatory>"
	}

It's a synchronous request, which means it can also be triggered via Admin API, which makes it easy to "fire" via, e.g., a curl one-liner:

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "destroy", "name": "lorenzo" } }' http://localhost:7088/admin

A successful processing of the request will look like this:

	{
		"ndi": "success"
	}

### translate

As explained in a previous section, the Janus NDI Plugin expects an SDP offer to kickstart the WebRTC-to-NDI translation: this process is made possible by the `translate` request itself, which needs to include the WebRTC SDP offer itself, and some details on the NDI translation to perform.

The only mandatory argument in the `translate` request is `name`, which specifies which name the NDI sender will need to use: this is how NDI consumers will identify the streams when listing available sources. If this name refers to an NDI sender previously created with `create`, then the stream will be sent there, otherwise a new NDI sender will be created from scratch: in the latter case, the NDI sender will also be automatically destroyed when the PeerConnection is closed. NDI metadata can also be sent, optionally, by providing the XML data to advertise in the `metadata` property.

By default the WebRTC stream will be translated "as is" to NDI: this means that, if the video resolution changes during the session (which browsers can do in response to CPU usage or RTCP feedback), then the same resolution changes will be visible in the NDI stream too. While NDI applications do have a way to "lock" resolutions, it may sometimes be helpful to enforce a static resolution from the source itself: this is something you can do via the optional `width` and `height` arguments, that if set will force the plugin to always scale the incoming video to the provided resolution, thus providing NDI consumers with a consistent feed; notice that this scaling procedure does NOT take aspect ratio into account, which means that if the resolution provided has a different aspect ration than the actual video, the video will be stretched. An `fps` can be provided as well, which is only informational though, as it's advertised when sending packets but not enforced.

Finally, a `strict` boolean can specify whether the "strict mode" should be enforced when decoding videos. By default, the decoder is more tolerant, and so will accept broken frames which will result in a smoother experience, but also in occasional video artifacts in case of unrecovered packet losses; enabling "strict mode" will discard frames where packets have been detected as missing, thus resulting in video freezes when that happens, until a keyframe recovers the picture.

The format of the `translate` request is the following:

	{
		"request": "translate",
		"name": "<unique name to use for the NDI sender; mandatory>",
		"metadata": "<NDI metadata to send; optional>",
		"width": <width to forcibly scale the video to; optional>,
		"height": <height to forcibly scale the video to; optional>,
		"fps": <FPS to advertise via NDI; optional>,
		"strict": <whether strict mode should be enforced when decoding video; optional, false by default>,
		"ondisconnect": {	// Optional image to show when the user disconnects (assuming no placeholder is used)
			"image": "<local or web path to an image to send at the end; mandatory if ondisconnect is used>",
			"color": "<color to use as background (#RRGGBB format), in case aspect ratio doesn't match; optional>"
		},
		"videocodec": "<video codec to force; optional>
	}

The `translate` request is asynchronous, which means that, from a Janus API perspective, you'll always receive an `ack` first, and an event later on, with info on whether the request was successful or not. In case an SDP answer has been prepared, the event will look like this:

	{
		"event": "translating",
		"warning": "<optional verbose description of something that should be taken into account>"
	}

Please refer to the official Janus API documentation for info on how SDP offers and answers are exchanged with plugins, if you find this documentation lacking in that regard.

### configure

After a WebRTC-to-NDI session has been created, and an NDI translation is taking place, there are a few things you can tweak by means of a `configure` request. This includes:

* a way to programmatically ask for a keyframe via RTCP PLI;
* a way to send a bitrate cap via RTCP REMB;
* a way to pause/resume the NDI translation temporarily.

Neither the PLI nor REMB requests should ever be needed, as (i) the plugin already automatically asks for a keyframe when some decode errors take place, and (ii) since the plugin will most of the times not be talking to browsers directly, but other WebRTC servers instead, good chances are that any REMB feedback they may send will simply be ignored. Notice that pausing an NDI translation will start sending the placeholder image, if the NDI sender was pre-created: resuming the translation will restore the live video.

The format of the `configure` request is the following:

	{
		"request": "configure",
		"keyframe": <if set to true, will trigger a RTCP PLI message; optional>,
		"bitrate": <bitrate to send back via a RTCP REMB message; optional>,
		"paused": <true|false, whether the NDI translation for this user should be paused; optional>
	}

The `configure` request is asynchronous, which means that, from a Janus API perspective, you'll always receive an `ack` first, and an event later on, which in this case will look like this:

	{
		"event": "configured"
	}

### hangup

An existing WebRTC-to-NDI session can be explicitly torn down using the `hangup` request. Notice that it's not strictly necessary to invoke this message for tnat purpose: the plugin will also automatically be informed if the WebRTC PeerConnection has been closed via other means, e.g., a DTLS alert, and Janus detected it. Nevertheless, an explicit mechanism to do that might be helpful too, which is what this request is for: upon reception of a `hangup` request, the Janus NDI Plugin will ask the Janus core to forcibly tear down the PeerConnection, and thus trigger the cleanup mechanisms.

Closing a WebRTC PeerConnection will release the resources allocated for the translation, and will also destroy the NDI sender associated with it.

The `hangup` request has no arguments, so its format is straightforward:

	{
		"request": "hangup"
	}

The `hangup` request is asynchronous, which means that, from a Janus API perspective, you'll always receive an `ack` first, and an event later on, which in this case will look like this:

	{
		"event": "hangingup"
	}

### start_test_pattern

Considering that the Janus NDI Plugin, as all NDI applications, uses mDNS to advertise the streams, it makes sense to have ways to first of all verify whether or not it will be visible from NDI consumers, without needing an ad-hoc WebRTC session for the purpose. This is exactly what the `start_test_pattern` request is for: in fact, its only purpose is to use a hardcoded image as a source for a static video pattern to send via NDI. More precisely, when the test pattern is started, the NDI plugin creates a new NDI sender called `janus-ndi-test`, and starts sending the image over and over at about 30fps. If NDI consumers are able to find the stream and receive it correctly, then it can be assumed that WebRTC-to-NDI sessions will work correctly as well.

Notice that, since the same name is always used, the test pattern can only be started once at a time: attempts to start a second test pattern while one is already running is an error. You'll need to stop the previous one first.

The `start_test_pattern` request has no arguments, so its format is straightforward:

	{
		"request": "start_test_pattern"
	}

It's a synchronous request, which means it can also be triggered via Admin API, which makes it easy to "fire" via, e.g., a curl one-liner:

	curl -d '{ "janus": "message_plugin", "transaction": "123", "admin_secret": "janusoverlord", "plugin": "janus.plugin.ndi", "request": { "request": "start_test_pattern" } }' http://localhost:7088/admin

A successful processing of the request will look like this:

	{
		"ndi": "success"
	}

### stop_test_pattern

The `stop_test_pattern` request is used to interrupt the delivery of a test pattern as started via a `start_test_pattern` request. The same considerations apply here as well.
