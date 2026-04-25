import { createServer } from './wire/server.js';

const port = Number(process.env.PORT ?? 27017);
createServer(port);
