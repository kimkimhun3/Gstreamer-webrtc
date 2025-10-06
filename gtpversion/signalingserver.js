'use strict';

const port = process.env.PORT || 8080;
const htmlDir = 'public';

const express = require('express');
const app = express();
const expressWs = require('express-ws')(app);

const clients = new Map();   // id -> ws
let index = 0;

function makeId() {
  const alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789';
  let out = '';
  for (let i = 0; i < 9; i++) out += alphabet[Math.floor(Math.random() * alphabet.length)];
  return out;
}

function broadcast(senderId, obj) {
  const msg = JSON.stringify(obj);
  for (const [id, ws] of clients) {
    if (id !== senderId && ws.readyState === 1) {
      try { ws.send(msg); } catch (e) { /* ignore */ }
    }
  }
}

function routeOrBroadcast(senderId, obj, to) {
  const msg = JSON.stringify(obj);
  if (to && clients.has(to)) {
    const ws = clients.get(to);
    if (ws && ws.readyState === 1) {
      try { ws.send(msg); } catch (e) { /* ignore */ }
      return;
    }
  }
  broadcast(senderId, obj);
}

app.ws('/', function(ws, req) {
  const clientId = makeId();
  clients.set(clientId, ws);
  console.log('Client connected:', clientId, 'Total clients:', clients.size);

  // tell the client their id
  ws.send(JSON.stringify({ type: 'registered', id: clientId }));

  ws.on('message', function(message) {
    let data = null;
    try { data = JSON.parse(message); } catch { return; }
    const type = data.type;

    // for logging
    console.log('Received from', clientId + ':', type);

    switch (type) {
      case 'join': {
        // currently unused; kept for future room/role logic
        // data.room, data.clientType could be stored here
        break;
      }

      case 'request-offer': {
        // ask senders to create a fresh offer; broadcast is fine
        broadcast(clientId, { type: 'request-offer', from: clientId });
        break;
      }

      case 'offer': {
        // route if "to" is known, else broadcast (keeps compatibility with your C++ sender)
        routeOrBroadcast(clientId, {
          type: 'offer',
          from: clientId,
          sdp: data.sdp
        }, data.to);
        break;
      }

      case 'answer': {
        routeOrBroadcast(clientId, {
          type: 'answer',
          from: clientId,
          sdp: data.sdp
        }, data.to);
        break;
      }

      case 'ice-candidate': {
        routeOrBroadcast(clientId, {
          type: 'ice-candidate',
          from: clientId,
          candidate: data.candidate
        }, data.to);
        break;
      }

      default:
        // ignore unknown
        break;
    }
  });

  ws.on('close', function() {
    clients.delete(clientId);
    console.log('Client disconnected:', clientId, 'Total clients:', clients.size);
    broadcast(clientId, { type: 'peer-left', id: clientId, from: clientId });
  });
});

app.use(express.static(htmlDir));
app.listen(port, () => {
  console.log('Signaling server running on port', port);
  console.log('WebSocket server ready');
});
