// We import the settings.js file to know which address we should contact
// to talk to Janus, and optionally which STUN/TURN servers should be
// used as well. Specifically, that file defines the "server" and
// "iceServers" properties we'll pass when creating the Janus session.

var janus = null;
var ndi = null;
var opaqueId = "ndi-"+Janus.randomString(12);

var vcodec = (getQueryStringValue("vcodec") !== "" ? getQueryStringValue("vcodec") : null);
var localTracks = {}, localVideos = 0;

$(document).ready(function() {
	// Initialize the library (all console debuggers enabled)
	Janus.init({debug: "all", callback: function() {
		// Use a button to start the demo
		$('#start').one('click', function() {
			$(this).attr('disabled', true).unbind('click');
			// Make sure the browser supports WebRTC
			if(!Janus.isWebrtcSupported()) {
				bootbox.alert("No WebRTC support... ");
				return;
			}
			// Create session
			janus = new Janus(
				{
					server: server,
					success: function() {
						// Attach to NDI plugin
						janus.attach(
							{
								plugin: "janus.plugin.ndi",
								opaqueId: opaqueId,
								success: function(pluginHandle) {
									$('#details').remove();
									ndi = pluginHandle;
									Janus.log("Plugin attached! (" + ndi.getPlugin() + ", id=" + ndi.getId() + ")");
									// We're connected to the plugin, show the settings
									$('#videos').removeClass('hide');
									$('#name').removeAttr('disabled');
									$('#start').removeAttr('disabled').html("Stop")
										.click(function() {
											$(this).attr('disabled', true);
											janus.destroy();
										});
								},
								error: function(error) {
									console.error("  -- Error attaching plugin...", error);
									bootbox.alert("Error attaching plugin... " + error);
								},
								consentDialog: function(on) {
									Janus.debug("Consent dialog should be " + (on ? "on" : "off") + " now");
									if(on) {
										// Darken screen and show hint
										$.blockUI({
											message: '<div><img src="up_arrow.png"/></div>',
											css: {
												border: 'none',
												padding: '15px',
												backgroundColor: 'transparent',
												color: '#aaa',
												top: '10px',
												left: (navigator.mozGetUserMedia ? '-100px' : '300px')
											} });
									} else {
										// Restore screen
										$.unblockUI();
									}
								},
								iceState: function(state) {
									Janus.log("ICE state changed to " + state);
								},
								mediaState: function(medium, on) {
									Janus.log("Janus " + (on ? "started" : "stopped") + " receiving our " + medium);
								},
								webrtcState: function(on) {
									Janus.log("Janus says our WebRTC PeerConnection is " + (on ? "up" : "down") + " now");
									$('#videoright').parent().parent().unblock();
								},
								slowLink: function(uplink, lost) {
									Janus.warn("Janus reports problems " + (uplink ? "sending" : "receiving") +
										" packets on this PeerConnection (" + lost + " lost packets)");
								},
								onmessage: function(msg, jsep) {
									Janus.debug(" ::: Got a message :::", msg);
									if(msg.error) {
										bootbox.alert(msg.error);
									}
									if(jsep) {
										Janus.debug("Handling SDP as well...", jsep);
										ndi.handleRemoteJsep({ jsep: jsep });
									}
									// Tally update?
									if(msg.result) {
										if(msg.result.preview === true) {
											$('#preview').removeClass('hide');
										} else if(msg.result.preview === false) {
											$('#preview').addClass('hide');
										}
										if(msg.result.program === true) {
											$('#program').removeClass('hide');
										} else if(msg.result.program === false) {
											$('#program').addClass('hide');
										}
									}
								},
								onlocaltrack: function(track, on) {
									Janus.debug("Local track " + (on ? "added" : "removed") + ":", track);
									// We use the track ID as name of the element, but it may contain invalid characters
									let trackId = track.id.replace(/[{}]/g, "");
									if(!on) {
										// Track removed, get rid of the stream and the rendering
										let stream = localTracks[trackId];
										if(stream) {
											try {
												let tracks = stream.getTracks();
												for(let i in tracks) {
													let mst = tracks[i];
													if(mst !== null && mst !== undefined)
														mst.stop();
												}
											} catch(e) {}
										}
										if(track.kind === "video") {
											$('#myvideo' + trackId).remove();
											localVideos--;
											if(localVideos === 0) {
												// No video, at least for now: show a placeholder
												if($('#videoright .no-video-container').length === 0) {
													$('#videoright').prepend(
														'<div class="no-video-container">' +
															'<i class="fa-solid fa-video fa-xl no-video-icon"></i>' +
															'<span class="no-video-text">No webcam available</span>' +
														'</div>');
												}
											}
										}
										delete localTracks[trackId];
										return;
									}
									// If we're here, a new track was added
									let stream = localTracks[trackId];
									if(stream) {
										// We've been here already
										return;
									}
									$('#videos').removeClass('hide').removeClass('hide');
									if(track.kind === "audio") {
										// We ignore local audio tracks, they'd generate echo anyway
										if(localVideos === 0) {
											// No video, at least for now: show a placeholder
											if($('#videoright .no-video-container').length === 0) {
												$('#videoright').prepend(
													'<div class="no-video-container">' +
														'<i class="fa-solid fa-video fa-xl no-video-icon"></i>' +
														'<span class="no-video-text">No webcam available</span>' +
													'</div>');
											}
										}
									} else {
										// New video track: create a stream out of it
										localVideos++;
										$('#videoright .no-video-container').remove();
										stream = new MediaStream([track]);
										localTracks[trackId] = stream;
										Janus.log("Created local stream:", stream);
										Janus.log(stream.getTracks());
										Janus.log(stream.getVideoTracks());
										$('#videoright').prepend('<video class="rounded centered" id="myvideo' + trackId + '" width=100% autoplay playsinline muted="muted"/>');
										Janus.attachMediaStream($('#myvideo' + trackId).get(0), stream);
									}
									if(ndi.webrtcStuff.pc.iceConnectionState !== "completed" &&
											ndi.webrtcStuff.pc.iceConnectionState !== "connected") {
										$('#videoright').parent().parent().block({
											message: '<b>Publishing...</b>',
											css: {
												border: 'none',
												backgroundColor: 'transparent',
												color: 'white'
											}
										});
									}
								},
								oncleanup: function() {
									Janus.log(" ::: Got a cleanup notification :::");
									$('#myvideo').remove();
									$('#waitingvideo').remove();
									$("#videoright").parent().parent().unblock();
									$('#name').removeAttr('disabled');
								}
							});
					},
					error: function(error) {
						Janus.error(error);
						bootbox.alert(error, function() {
							window.location.reload();
						});
					},
					destroyed: function() {
						window.location.reload();
					}
				});
		});
	}});
});

function checkEnter(event) {
	let theCode = event.keyCode ? event.keyCode : event.which ? event.which : event.charCode;
	if(theCode == 13) {
		publishMedia();
		return false;
	} else {
		return true;
	}
}

function publishMedia() {
	let ndiname = $('#name').val();
	if(!ndiname || ndiname === '')
		return;
	$('#name').attr('disabled', true);
	// Publish audio and video
	ndi.createOffer(
		{
			tracks: [
				{ type: 'audio', capture: true, recv: false },
				{ type: 'video', capture: true, recv: false }
			],
			success: function(jsep) {
				Janus.debug("Got SDP!", jsep);
				// Send a request to the plugin
				let translate = {
					request: "translate",
					// If an NDI sender wasn't previously created with "create"
					// (which allows the NDI sender to survive tearing down
					// the PeerConnection) it will be created now here
					name: ndiname,
					// To force scaling to a specific resolution (e.g., to avoid
					// the NDI video becoming smaller because of a decreasing
					// resolution in the WebRTC stream), set width and height too
					//~ 	width: 640,
					//~ 	height: 480,
					// If you know the FPS of the video (or what you asked for),
					// you can tell the NDI plugin to better inform recipients
					//~ 	fps: 30,
					// You can provide optional NDI metadata as well
					//~ 	metadata: '<ndi product_name="Broadcaster" live_stream="Lorenzo" />',
					// To send a static image as the last frame before disconnecting,
					// you can specify the path to the image and the background color
					//~ 	ondisconnect: {
					//~ 		image: "https://www.meetecho.com/en/img/meetecho.png",
					//~ 		color: "#FFFFFF"
					//~ 	},
					// To force the decoder to drop a frame in case some packets
					// result missing (looking at sequence numbers), you need
					// to set the strict property to true (it's false by default)
					//~ 	strict: true
				}
				if(vcodec)
					translate["videocodec"] = vcodec;
				ndi.send({ message: translate, jsep: jsep });
			},
			error: function(error) {
				Janus.error("WebRTC error:", error);
				bootbox.alert("WebRTC error... " + error.message);
			}
		});
}

// Helper to parse query string
function getQueryStringValue(name) {
	name = name.replace(/[\[]/, "\\[").replace(/[\]]/, "\\]");
	let regex = new RegExp("[\\?&]" + name + "=([^&#]*)"),
		results = regex.exec(location.search);
	return results === null ? "" : decodeURIComponent(results[1].replace(/\+/g, " "));
}
