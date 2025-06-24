'use strict';

import Handle from 'janode/handle';

/* The plugin ID exported in the plugin descriptor */
const PLUGIN_ID = 'janus.plugin.ndi';

/* These are the requests defined for the Janus NDI plugin API */
const REQUEST_CREATE = 'create';
const REQUEST_UPDATE_IMG = 'update_img';
const REQUEST_LIST = 'list';
const REQUEST_DESTROY = 'destroy';
const REQUEST_TRANSLATE = 'translate';
const REQUEST_CONFIGURE = 'configure';
const REQUEST_HANGUP = 'hangup';
const REQUEST_START_TEST_PATTERN = 'start_test_pattern';
const REQUEST_STOP_TEST_PATTERN = 'stop_test_pattern';

/* These are the events/responses that the Janode plugin will manage */
/* Some of them will be exported in the plugin descriptor */
const PLUGIN_EVENT = {
	LIST: 'ndi_list',
	TRANSLATING: 'ndi_translating',
	CONFIGURED: 'ndi_configured',
	TALLY: 'ndi_tally',
	HANGINGUP: 'ndi_hangingup',
	SUCCESS: 'ndi_success',
	ERROR: 'ndi_error',
};

/* The class implementing the Janus NDI plugin (https://github.com/meetecho/janus-ndi/blob/main/docs/API.md) */
class JanusNdiHandle extends Handle {
	/* Constructor */
	constructor(session, id) {
		super(session, id);

		/* NDI sender associated to this handle, when active */
		this.name = null;
	}

	/* The custom "handleMessage" needed for handling Janus NDI plugin messages */
	handleMessage(janus_message) {
		const { plugindata, transaction } = janus_message;
		if(plugindata && plugindata.data && plugindata.data.ndi) {
			const message_data = plugindata.data;
			const { ndi, error, error_code, name } = message_data;

			/* Prepare an object for the output Janode event */
			const janode_event = this._newPluginEvent(janus_message);

			/* Add the NDI sender name, if available */
			if(name)
				janode_event.data.name = name;

			/* The plugin will emit an event only if the handle does not own the transaction */
			/* That means that a transaction has already been closed or this is an async event */
			const emit = (this.ownsTransaction(transaction) === false);

			switch(ndi) {
				/* Success response */
				case 'success':
					/* Senders list API */
					if(typeof message_data.list !== 'undefined') {
						janode_event.data.list = message_data.list;
						janode_event.event = PLUGIN_EVENT.LIST;
						break;
					}
					/* In this case the "ndi" field of the Janode event is "success" */
					janode_event.event = PLUGIN_EVENT.SUCCESS;
					break;

				/* Error response */
				case 'error':
					/* Janus NDI plugin error */
					janode_event.event = PLUGIN_EVENT.ERROR;
					janode_event.data = new Error(`${error_code} ${error}`);
					/* In case of error, close a transaction */
					this.closeTransactionWithError(transaction, janode_event.data);
					break;

				/* Generic event (including asynchronous errors) */
				case 'event':
					/* Janus NDI plugin error */
					if(error) {
						janode_event.event = PLUGIN_EVENT.ERROR;
						janode_event.data = new Error(`${error_code} ${error}`);
						/* In case of error, close a transaction */
						this.closeTransactionWithError(transaction, janode_event.data);
						break;
					}
					/* Asynchronous success for this handle */
					if(typeof message_data.result !== 'undefined') {
						const { event } = message_data.result;
						switch(event) {
							/* NDI sender translation started  */
							case 'translating':
								janode_event.event = PLUGIN_EVENT.TRANSLATING;
								janode_event.data.name = name;
								break;

							/* WebRTC PeerConnection configured */
							case 'configured':
								janode_event.event = PLUGIN_EVENT.CONFIGURED;
								janode_event.data.name = name;
								break;

							/* NDI tally information available */
							case 'tally':
								janode_event.event = PLUGIN_EVENT.TALLY;
								janode_event.data.name = message_data.result.name;
								janode_event.data.preview = message_data.result.preview;
								janode_event.data.program = message_data.result.program;
								break;
							
						}
					}
					break;
			}

			/* The event has been handled */
			if(janode_event.event) {
				/* Try to close the transaction */
				this.closeTransactionWithSuccess(transaction, janus_message);
				/* If the transaction was not owned, emit the event */
				if(emit)
					this.emit(janode_event.event, janode_event.data);
				return janode_event;
			}
		}

		/* The event has not been handled, return a falsy value */
		return null;
	}

	/*
	 * 
	 * These are the APIs that users need to work with the Janus NDI plugin
	 * 
	 */

	/* Pre-create a reusable NDI sender */
	async create({ name, placeholder, width, height, keep_ratio }) {
		const body = {
			request: REQUEST_CREATE,
			name
		};
		if(typeof placeholder === 'string')
			body.placeholder = placeholder;
		if(typeof width === 'number')
			body.width = width;
		if(typeof height === 'number')
			body.height = height;
		if(typeof keep_ratio === 'boolean')
			body.keep_ratio = keep_ratio;

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.SUCCESS)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Update the placeholder image for an existing NDI sender */
	async updateImg({ name, image, width, height, keep_ratio }) {
		const body = {
			request: REQUEST_UPDATE_IMG,
			name,
			image
		};
		if(typeof width === 'number')
			body.width = width;
		if(typeof height === 'number')
			body.height = height;
		if(typeof keep_ratio === 'boolean')
			body.keep_ratio = keep_ratio;

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.SUCCESS)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* List available NDI senders */
	async list() {
		const body = {
			request: REQUEST_LIST,
		};

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.LIST)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Destroy a shared NDI sender */
	async destroy({ name }) {
		const body = {
		  request: REQUEST_DESTROY,
		  name,
		};

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.SUCCESS)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Setup a new WebRTC PeerConnection to translate to NDI */
	async translate({ name, metadata, width, height, fps, strict, onDisconnect, videocodec, jsep = null }) {
		const body = {
			request: REQUEST_TRANSLATE,
			name,
		};
		if(typeof metadata === 'string')
			body.metadata = metadata;
		if(typeof width === 'number')
			body.width = width;
		if(typeof height === 'number')
			body.height = height;
		if(typeof fps === 'number')
			body.fps = fps;
		if(typeof strict === 'boolean')
			body.strict = strict;
		if(typeof onDisconnect === 'object' && onDisconnect)
			body.ondisconnect = onDisconnect;
		if(typeof videocodec === 'string')
			body.videocodec = videocodec;

		const response = await this.message(body, jsep);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.TRANSLATING)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Configure an established WebRTC PeerConnection */
	async configure({ keyframe, bitrate, paused }) {
		const body = {
			request: REQUEST_CONFIGURE,
		};
		if(typeof keyframe === 'boolean')
			body.keyframe = keyframe;
		if(typeof bitrate === 'number')
			body.bitrate = bitrate;
		if(typeof paused === 'boolean')
			body.paused = paused;

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.CONFIGURED)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Hangup an NDI sender's WebRTC PeerConnection */
	async hangup() {
		const body = {
			request: REQUEST_HANGUP,
		};

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.HANGINGUP)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Start the NDI test pattern */
	async startTestPattern() {
		const body = {
			request: REQUEST_START_TEST_PATTERN,
		};

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.SUCCESS)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

	/* Stop the NDI test pattern */
	async stopTestPattern() {
		const body = {
			request: REQUEST_STOP_TEST_PATTERN,
		};

		const response = await this.message(body);
		const { event, data: evtdata } = this._getPluginEvent(response);
		if(event === PLUGIN_EVENT.SUCCESS)
			return evtdata;
		const error = new Error(`unexpected response to ${body.request} request`);
		throw(error);
	}

}

/* The exported plugin descriptor */
export default {
	id: PLUGIN_ID,
	Handle: JanusNdiHandle,

	EVENT: {
		/* NDI tally information */
		JANUS_NDI_TALLY: PLUGIN_EVENT.TALLY,

		/* Generic Janus NDI plugin error */
		JANUS_NDI_ERROR: PLUGIN_EVENT.ERROR,
	},
};
