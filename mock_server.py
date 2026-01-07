#!/usr/bin/env python3

from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse
import uvicorn
import httpx
import json
import sys
import os
import datetime
from pathlib import Path

app = FastAPI()

CONFIG_PATH = Path("server_config.json")
TEMPER_PATH = Path("temper")

def load_config():
    if CONFIG_PATH.exists():
        with open(CONFIG_PATH, 'r') as f:
            return json.load(f)
    return {
        "intercept_log": False,
        "passthrough": True,
        "temper": True,
        "host_server_target": "https://mt.racers.co.kr/",
        "web_server_target": "https://www.racers.co.kr"
    }

def load_templates():
    templates = {}

    if TEMPER_PATH.exists() and TEMPER_PATH.is_dir():
        for json_file in TEMPER_PATH.glob("*.json"):
            try:
                with open(json_file, 'r') as f:
                    file_templates = json.load(f)

                templates.update(file_templates)

            except Exception as e:
                print(f"Error loading template {json_file}: {e}")

    return templates

import re

def find_template(templates, method, path):
    exact_key = f"{method} {path}"
    if exact_key in templates:
        return exact_key, templates[exact_key]

    # Try wildcard matches
    for template_key, template in templates.items():
        if '/*' in template_key:
            # Split template key into method and path pattern
            template_parts = template_key.split(' ', 1)
            if len(template_parts) == 2:
                template_method, template_path_pattern = template_parts

                # Check if methods match
                if template_method == method:
                    # Convert wildcard pattern to regex
                    # Escape special regex chars and replace * with .*
                    pattern = re.escape(template_path_pattern).replace(r'\*', '.*')
                    try:
                        if re.match(f"^{pattern}$", path):
                            return template_key, template
                    except re.error:
                        # Invalid regex pattern, skip
                        continue

    return None, None

def create_server_app(server_type="host"):
    app = FastAPI()

    @app.middleware("http")
    async def intercept_requests(request: Request, call_next):
        global config, templates

        config = load_config()
        templates = load_templates()

        request_body = None
        if request.method in ['POST', 'PUT', 'PATCH']:
            try:
                request_body = await request.body()
            except:
                request_body = b''

        template_key, template = None, None
        if config.get("temper", True):
            template_key, template = find_template(templates, request.method, request.url.path)

        if template and template.get("silence", False):
            silenced_response = template.get("response", {"status": "silenced", "message": "Request silenced"})
            silenced_status = template.get("status_code", 200)
            return JSONResponse(content=silenced_response, status_code=silenced_status)

        if config.get("intercept_log", False):
            timestamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
            full_url = str(request.url).replace(str(request.base_url), '/')
            print(f"[{timestamp}] [{server_type.upper()}] {request.method} {full_url}")

            headers_dict = dict(request.headers)
            print(f"[{timestamp}] Headers: {headers_dict}")

            if request_body:
                print(f"[{timestamp}] Body: {request_body.decode('utf-8', errors='ignore')}")

        # Get response - either from passthrough or default
        response_content = None
        response_status = 200
        response_headers = {}

        if config.get("passthrough", True):
            try:
                headers = dict(request.headers)

                if server_type == "host":
                    target_url = config.get("host_server_target", "https://mt.racers.co.kr/")
                    headers['host'] = 'mt.racers.co.kr'
                else:
                    target_url = config.get("web_server_target", "https://www.racers.co.kr")
                    headers['host'] = 'www.racers.co.kr'

                url = f"{target_url.rstrip('/')}{request.url.path}"
                if request.url.query:
                    url += f"?{request.url.query}"

                if config.get("intercept_log", False):
                    print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Passthrough to: {url}")

                async with httpx.AsyncClient(timeout=30.0) as client:
                    real_response = await client.request(
                        method=request.method,
                        url=url,
                        headers=headers,
                        content=request_body
                    )

                    if config.get("intercept_log", False):
                        print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Real server response: {real_response.status_code}")

                    response_content = real_response.content
                    response_status = real_response.status_code
                    response_headers = dict(real_response.headers)

            except Exception as e:
                if config.get("intercept_log", False):
                    print(f"[{datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] Passthrough error: {e}")
                response_content = b'{"status": "ok"}'
                response_status = 200
                response_headers = {"content-type": "application/json"}
        else:
            response_content = b'{"status": "ok"}'
            response_status = 200
            response_headers = {"content-type": "application/json"}

        # Apply template override if template exists (and not silenced)
        if template:
            template_response = template["response"]
            template_status = template.get("status_code", 200)

            return JSONResponse(content=template_response, status_code=template_status)

        # Return the response (either from passthrough or default)
        return Response(
            content=response_content,
            status_code=response_status,
            headers=response_headers
        )

    @app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"])
    async def catch_all(path: str = ""):
        """Fallback route"""
        return JSONResponse(content={"status": "ok"})

    return app

def run_server(server_type, port):
    """Run a server of the specified type on the given port"""
    app = create_server_app(server_type)
    print(f"🚀 Starting {server_type.upper()} server on port {port}")

    uvicorn.run(app, host="0.0.0.0", port=port, log_level="error")

if __name__ == '__main__':
    import threading

    # Load initial config
    config = load_config()

    print("🚀 Starting Dual Server Setup")
    print(f"📝 Logging: {'enabled' if config.get('intercept_log') else 'disabled'}")
    print(f"🔄 Passthrough: {'enabled' if config.get('passthrough') else 'disabled'}")
    print(f"🎯 Templates: {'enabled' if config.get('temper') else 'disabled'}")
    print()

    # Create and start host server (port 81)
    host_thread = threading.Thread(
        target=run_server,
        args=("host", 81),
        daemon=True
    )
    host_thread.start()

    # Create and start web server (port 82)
    web_thread = threading.Thread(
        target=run_server,
        args=("web", 82),
        daemon=True
    )
    web_thread.start()

    print("✅ Both servers started!")
    print("🌐 Host Server (mh.hostServerUrl): http://localhost:81 -> https://mt.racers.co.kr/")
    print("🌐 Web Server (mh.webServerUrl): http://localhost:82 -> https://www.racers.co.kr")
    print("Press Ctrl+C to stop both servers")
    print()

    try:
        # Keep main thread alive
        while True:
            import time
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n🛑 Shutting down servers...")
        exit(0)
