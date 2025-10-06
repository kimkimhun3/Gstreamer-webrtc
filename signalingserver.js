const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

// Serve static files (HTML client)
app.use(express.static('public'));

// Store connected clients
const clients = new Map();

wss.on('connection', (ws) => {
  const clientId = generateId();
  clients.set(clientId, ws);
  
  console.log(`Client connected: ${clientId}`);
  console.log(`Total clients: ${clients.size}`);
  
  // Send client their ID
  ws.send(JSON.stringify({
    type: 'registered',
    id: clientId
  }));

  ws.on('message', (message) => {
    try {
      const data = JSON.parse(message);
      console.log(`Received from ${clientId}:`, data.type);
      
      // Handle different message types
      switch(data.type) {
        // Ask other peers (e.g., the sender) to create a fresh offer
        case 'request-offer':
          // broadcast(clientId, data);
           broadcast(clientId, { type: 'request-offer', from: clientId });
          break;

        case 'offer':
          // Forward offer to all other clients (broadcast)
          broadcast(clientId, data);
          break;
          
        case 'answer':
          // Forward answer to specific peer
          if (data.to && clients.has(data.to)) {
            const targetWs = clients.get(data.to);
            targetWs.send(JSON.stringify({
              type: 'answer',
              from: clientId,
              sdp: data.sdp
            }));
          }
          break;
          
        case 'ice-candidate':
          // Forward ICE candidate
          if (data.to) {
            if (clients.has(data.to)) {
              const targetWs = clients.get(data.to);
              targetWs.send(JSON.stringify({
                type: 'ice-candidate',
                from: clientId,
                candidate: data.candidate
              }));
            }
          } else {
            // Broadcast to all if no specific target
            broadcast(clientId, {
              type: 'ice-candidate',
              from: clientId,
              candidate: data.candidate
            });
          }
          break;
          
        case 'ping':
          ws.send(JSON.stringify({ type: 'pong' }));
          break;
          
        default:
          console.log('Unknown message type:', data.type);
      }
    } catch (e) {
      console.error('Error parsing message:', e);
    }
  });

  ws.on('close', () => {
    console.log(`Client disconnected: ${clientId}`);
    clients.delete(clientId);
    // Let others know this peer left
    broadcast(clientId, { type: 'peer-left', id: clientId });
    console.log(`Total clients: ${clients.size}`);
  });

  ws.on('error', (error) => {
    console.error(`WebSocket error for ${clientId}:`, error);
  });
});

// Broadcast message to all clients except sender
function broadcast(senderId, data) {
  const message = JSON.stringify({
    ...data,
    from: senderId
  });
  
  clients.forEach((client, id) => {
    if (id !== senderId && client.readyState === WebSocket.OPEN) {
      client.send(message);
    }
  });
}

function generateId() {
  return Math.random().toString(36).substr(2, 9);
}

const PORT = process.env.PORT || 8080;
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Signaling server running on port ${PORT}`);
  console.log(`WebSocket server ready`);
});
