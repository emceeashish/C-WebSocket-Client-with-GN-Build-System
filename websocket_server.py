import asyncio
import websockets

async def echo(websocket, path):
    async for message in websocket:
        print(f"Received: {message}")
        await websocket.send(message)  # Echo the message back

async def main():
    server = await websockets.serve(echo, "localhost", 8765)
    print("WebSocket server started at ws://localhost:8765")
    await server.wait_closed()

if __name__ == "__main__":
    asyncio.run(main())  # ✅ Fix: Use `asyncio.run()` to start the event loop
