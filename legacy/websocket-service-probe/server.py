#!/usr/bin/env python3

import asyncio
import datetime
import json
import websockets


DEVICE_STATE = {
    "status": "AUTHENTICATED",
    "temperature": "36.65",
    "temperature2": "36.05",
    "type": "Device-Get-State-Response"
}


def now():
    return datetime.datetime.now().isoformat(
        timespec="milliseconds"
    )


async def transmit(ws, data):

    text = json.dumps(
        data,
        separators=(",", ":")
    )

    print(
        f"{now()}  OUT  {text}",
        flush=True
    )

    await ws.send(text)


async def client(ws, path=None):

    print(
        f"{now()}  CONNECT {ws.remote_address}",
        flush=True
    )

    #
    # Real service pushes state shortly after connection.
    #
    await transmit(ws, DEVICE_STATE)

    try:

        async for message in ws:

            print(
                f"{now()}  IN   {message}",
                flush=True
            )

            try:
                obj = json.loads(message)

            except Exception as exc:

                print(
                    f"{now()}  INVALID JSON {exc}",
                    flush=True
                )

                continue

            msg_type = obj.get("type")

            print(
                f"{now()}  TYPE {msg_type!r}",
                flush=True
            )

            #
            # Confirmed real protocol.
            #
            if msg_type == "Device-Get-State":

                await transmit(
                    ws,
                    DEVICE_STATE
                )

                continue

            #
            # Everything else is deliberately logged
            # rather than guessed.
            #
            print(
                f"{now()}  *** NEW COMMAND DISCOVERED ***",
                flush=True
            )


    except websockets.exceptions.ConnectionClosed as exc:

        print(
            f"{now()}  DISCONNECT "
            f"code={exc.code} "
            f"reason={exc.reason}",
            flush=True
        )


async def main():

    print(
        f"{now()} Fake KineoDeviceService bridge starting",
        flush=True
    )

    async with websockets.serve(
        client,
        host=None,
        port=9002,
        max_size=32 * 1024 * 1024,
        ping_interval=20,
        ping_timeout=20
    ):

        print(
            f"{now()} LISTENING ws://localhost:9002",
            flush=True
        )

        await asyncio.Future()


asyncio.run(main())
