const fs        = require('fs');
const http      = require('http');
const WebSocket = require('ws');
const url       = require('url');

const VIDEO_TX_STREAM_PORT = 19650;    // ffmpeg pushes here http://127.0.0.1:19650/01
const VIDEO_TX_WEBSOCKET_PORT = 10002; // html renderers pull from here ws://flex-assets.ltnglobal.com/stream/live?01

// Receive LTN Encoder status json messages
const ENCODER_STATUS_HTTP_RX_PORT = 13100;   // Unused
const ENCODER_STATUS_WEBSOCKET_PORT = 13200; // Unused

// Receive NIC Monitor status json messages    
// ltn encoder stats are pushed here http://127.0.0.1:13300/nicmonitor/01
const NIC_MONITOR_STATUS_HTTP_RX_PORT = 13300;
// Analyzer web-ui available here: http://192.168.2.45:13300/analyzer/index.html

const NIC_MONITOR_STATUS_WEBSOCKET_PORT = 13400;   // analyzer webui pulls from here: ws://192.168.2.45:13400/stream/live?01

process.on('uncaughtException', function(err) {
	// Handle the error safely
	console.log(`UnCaught Exception ${err}`)
	console.error(err)
})

const videoTXChannels = {
    '01': new Set (),
    '02': new Set (),
    '03': new Set (),
    '04': new Set (),
    '05': new Set (),
    '06': new Set (),
    '07': new Set (),
    '08': new Set (),
	'09': new Set (),
	'10': new Set (),
	'11': new Set (),
	'12': new Set (),
	'13': new Set (),
	'14': new Set (),
	'15': new Set (),
	'16': new Set (),
}

function heartbeat() {
	this.isAlive = true;
}
  
// Websocket Server - transmit MPEG-TS to clients
var socketServer = new WebSocket.Server({port: VIDEO_TX_WEBSOCKET_PORT, host: "0.0.0.0", perMessageDeflate: false});
socketServer.connectionCount = 0;

socketServer.on('connection', function(socket, req) {
	socketServer.connectionCount++;
	socket.isAlive = true;
	socket.on('pong', heartbeat);

	// wss://flex-assets.ltnglobal.com/stream/live?5
	// where reqChannel becomes 5
    let reqChannel =  url.parse(req.url).query

	console.log('New Video websocket Streaming Connection ' + reqChannel + ' ' + socketServer.connectionCount)
    try {
		videoTXChannels[reqChannel].add(socket)
	} catch(e) {
		console.log('Tried to connect to undefined socket ' + e)
	}

	socket.on('close', function(code, message) {

		socketServer.connectionCount--;

		try {
	        videoTXChannels[reqChannel].delete(socket)
			console.log('del Connection ' + reqChannel + ' ' + socketServer.connectionCount)
		} catch(e) {
			console.log('Tried to disconnect to undefined socket ' + e)
		}

	});

});

const sendHeartbeat = setInterval(function ping() {

	socketServer.clients.forEach(function each(ws) {
	  if (ws.isAlive === false)
	  	return ws.terminate();
  
	  ws.isAlive = false;
	  ws.ping();

	});

}, 2000)

socketServer.broadcast = function(data) {

	socketServer.clients.forEach(function each(client) {
		if (client.readyState === WebSocket.OPEN) {
			client.send(data);
		}
	});

};

// HTTP Server to accept incomming MPEG-TS Stream from ffmpeg
// Eg. http://flex-assets.ltnglobal.com:19650/15
var streamServer = http.createServer( function(request, response) {

	var params = request.url.substring(1).split('/');

    console.log(`Starting video ingest`)
	console.log(
		'Stream Connected: ' +
		request.socket.remoteAddress + ':' +
		request.socket.remotePort
	);

	let frame = 0
	request.on('data', function(data) {

		// Push the incoming http mpegts to every registered websocket socket.
        videoTXChannels[params[0]].forEach((socket) => {
           
			if (socket.readyState === WebSocket.OPEN) {
				socket.send(data)
			}

        })
	
	});

	request.on('end',function() {
        console.log('FFMPEG Disconnect')
	});
})

// Keep the socket open for streaming
streamServer.timeout = 30000
streamServer.headersTimeout = 20000;
streamServer.listen(VIDEO_TX_STREAM_PORT);

// HTTP Server to accept incomming LTN encoder status json objects
// Distrubute all incoming json objects to all websocket listeners
// 
const encoderStatusChannels = {
    '01': new Set (),
    '02': new Set (),
    '03': new Set (),
    '04': new Set (),
    '05': new Set (),
    '06': new Set (),
    '07': new Set (),
    '08': new Set (),
	'09': new Set (),
	'10': new Set (),
	'11': new Set (),
	'12': new Set (),
	'13': new Set (),
	'14': new Set (),
	'15': new Set (),
	'16': new Set (),
}

var encoderStatusSocketServer = new WebSocket.Server({port: ENCODER_STATUS_WEBSOCKET_PORT, host: "0.0.0.0", perMessageDeflate: false});
encoderStatusSocketServer.connectionCount = 0;

encoderStatusSocketServer.on('connection', function(socket, req) {
	encoderStatusSocketServer.connectionCount++;
	socket.isAlive = true;
	socket.on('pong', heartbeat);
	
	// wss://flex-assets.ltnglobal.com/stream/live?5
	// where reqChannel becomes 5
    let reqChannel =  url.parse(req.url).query

	console.log('New Encoder Status Connection ' + reqChannel + ' ' + encoderStatusSocketServer.connectionCount)
    try {
		encoderStatusChannels[reqChannel].add(socket)
	} catch(e) {
		console.log('Tried to connect to undefined socket ' + e)
	}

	socket.on('close', function(code, message) {

		encoderStatusSocketServer.connectionCount--;

		try {
	        encoderStatusChannels[reqChannel].delete(socket)
			console.log('del Connection ' + reqChannel + ' ' + encoderStatusSocketServer.connectionCount)
		} catch(e) {
			console.log('Tried to disconnect to undefined socket ' + e)
		}

	});

});

const encoderStatusSendHeartbeat = setInterval(function ping() {

	encoderStatusSocketServer.clients.forEach(function each(ws) {
	  if (ws.isAlive === false)
	  	return ws.terminate();
  
	  ws.isAlive = false;
	  ws.ping();

	});

}, 2000)

encoderStatusSocketServer.broadcast = function(data) {

	encoderStatusSocketServer.clients.forEach(function each(client) {
		if (client.readyState === WebSocket.OPEN) {
			client.send(data);
		}
	});

};

var encoderStatusServer = http.createServer( function(request, response) {

	var params = request.url.substring(1).split('/');

    console.log(`Starting HTTP LTN Encoder Status relay`)
	console.log(
		'Stream Connected: ' +
		request.socket.remoteAddress + ':' +
		request.socket.remotePort
	);

	let frame = 0
	request.on('data', function(data) {

		// Push the incoming http mpegts to every registered websocket socket.
        encoderStatusChannels[params[0]].forEach((socket) => {
           
			if (socket.readyState === WebSocket.OPEN) {
				socket.send(data)
			}

        })
	
	});

	request.on('end',function() {
        console.log('FFMPEG Disconnect')
	});
})

// Keep the socket open for streaming
encoderStatusServer.timeout = 30000
encoderStatusServer.headersTimeout = 20000;
encoderStatusServer.listen(ENCODER_STATUS_HTTP_RX_PORT);

// HTTP Server to accept incomming LTN encoder status json objects
// Distrubute all incoming json objects to all websocket listeners
// 
const nicMonitorStatusChannels = {
    '01': new Set (),
    '02': new Set (),
    '03': new Set (),
    '04': new Set (),
    '05': new Set (),
    '06': new Set (),
    '07': new Set (),
    '08': new Set (),
	'09': new Set (),
	'10': new Set (),
	'11': new Set (),
	'12': new Set (),
	'13': new Set (),
	'14': new Set (),
	'15': new Set (),
	'16': new Set (),
}

var nicMonitorStatusSocketServer = new WebSocket.Server({port: NIC_MONITOR_STATUS_WEBSOCKET_PORT, host: "0.0.0.0", perMessageDeflate: false});
nicMonitorStatusSocketServer.connectionCount = 0;

nicMonitorStatusSocketServer.on('connection', function(socket, req) {
	nicMonitorStatusSocketServer.connectionCount++;
	socket.isAlive = true;
	socket.on('pong', heartbeat);
	
	// wss://flex-assets.ltnglobal.com/stream/live?5
	// where reqChannel becomes 5
    let reqChannel =  url.parse(req.url).query

	console.log('New Nic Monitor websocket Connection ' + reqChannel + ' ' + nicMonitorStatusSocketServer.connectionCount)
    try {
		nicMonitorStatusChannels[reqChannel].add(socket)
	} catch(e) {
		console.log('Tried to connect to undefined socket ' + e)
	}

	socket.on('close', function(code, message) {

		nicMonitorStatusSocketServer.connectionCount--;

		try {
	        nicMonitorStatusChannels[reqChannel].delete(socket)
			console.log('del Connection ' + reqChannel + ' ' + nicMonitorStatusSocketServer.connectionCount)
		} catch(e) {
			console.log('Tried to disconnect to undefined socket ' + e)
		}

	});

});

const nicMonitorStatusSendHeartbeat = setInterval(function ping() {

	nicMonitorStatusSocketServer.clients.forEach(function each(ws) {
	  if (ws.isAlive === false)
	  	return ws.terminate();
  
	  ws.isAlive = false;
	  ws.ping();

	});

}, 2000)

nicMonitorStatusSocketServer.broadcast = function(data) {

	nicMonitorStatusSocketServer.clients.forEach(function each(client) {
		if (client.readyState === WebSocket.OPEN) {
			client.send(data);
		}
	});

};

var nicMonitorStatusServer = http.createServer( function(request, response) {

	console.log(request.url)

	if (request.url.startsWith('/nicmonitor/')) {
		var params = request.url.substring(1).split('/');
		console.log(params)
		console.log(`Starting NIC Monitor Status relay`)
		console.log(
			'Nic Monitoring probe Connected: ' +
			request.socket.remoteAddress + ':' +
			request.socket.remotePort
		);

		request.on('data', function(data) {
			// Push the incoming json to every registered websocket socket.
			nicMonitorStatusChannels[params[1]].forEach((socket) => {
				if (socket.readyState === WebSocket.OPEN) {
					socket.send(data)
				}
			})
		});

		request.on('end',function() {
			console.log('tstools_nic_monitor disconnect')
			response.end()
		});
	} else
	if (request.url.startsWith('/encoder/')) {
		var params = request.url.substring(1).split('/');
		console.log(params)
		console.log(`Starting HTTP LTN Encoder Status relay`)
		console.log(
			'Stream Connected: ' +
			request.socket.remoteAddress + ':' +
			request.socket.remotePort
		);

		request.on('data', function(data) {
			encoderStatusChannels[params[1]].forEach((socket) => {
				if (socket.readyState === WebSocket.OPEN) {
					socket.send(data)
				}
			})
		});
		
		request.on('end',function() {
			console.log('ltn_encoder disconnect')
			response.end()
		});
	} else
	if (request.url.startsWith('/arcstream/')) {
		var params = request.url.substring(1).split('/');
		console.log(params)
		console.log(`Starting HTTP ARC Video relay`)
		console.log(
			'Stream Connected: ' +
			request.socket.remoteAddress + ':' +
			request.socket.remotePort
		);

		request.on('data', function(data) {
			videoTXChannels[params[1]].forEach((socket) => {
				if (socket.readyState === WebSocket.OPEN) {
					socket.send(data)
				}
			})
		});
		
		request.on('end',function() {
			console.log('encoder preview disconnect')
			response.end()
		});
	} else {
		var requestUrl = url.parse(request.url)    
		response.writeHead(200)
		console.log('reading ' + requestUrl.pathname)
		fs.createReadStream('html' + requestUrl.pathname).pipe(response)  // do NOT use fs's sync methods ANYWHERE on production (e.g readFileSync) 
	}
})

// Keep the socket open for streaming
nicMonitorStatusServer.timeout = 30000
nicMonitorStatusServer.headersTimeout = 20000;
nicMonitorStatusServer.listen(NIC_MONITOR_STATUS_HTTP_RX_PORT);

// ---
console.log(`Video Streaming Server Started. Listening for push requests on ${VIDEO_TX_STREAM_PORT}`)
console.log('Awaiting WebSocket MPEG-TS transmit ws://0.0.0.0:' + VIDEO_TX_WEBSOCKET_PORT + '/' );
console.log('Awaiting WebSocket LTN Encoder transmit ws://0.0.0.0:' + ENCODER_STATUS_WEBSOCKET_PORT + '/' );
console.log('Awaiting WebSocket NIC Monitor transmit ws://0.0.0.0:' + NIC_MONITOR_STATUS_WEBSOCKET_PORT + '/' );
