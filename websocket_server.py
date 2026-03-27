import asyncio
import websockets

async def echo(websocket):
    """Echo server — mirrors every message back to the client."""
    async for message in websocket:
        print(f"Received: {message}")
        await websocket.send(message)

async def main():
    async with websockets.serve(echo, "localhost", 8765):
        print("WebSocket echo server running at ws://localhost:8765")
        await asyncio.Future()  # Run forever

if __name__ == "__main__":
    asyncio.run(main())
