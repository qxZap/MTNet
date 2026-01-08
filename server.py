
from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse
import uvicorn
import httpx
import json
import sys
import os
import datetime
import asyncio
from pathlib import Path

app = FastAPI()

CONFIG_PATH = Path("server_config.json")
TEMPER_PATH = Path("temper")
TEMPER_CACHE_PATH = Path("temper_cache")

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

def load_cached_config():
    """Load cached config modifications"""
    cache_file = TEMPER_CACHE_PATH / "config_cache.json"
    if cache_file.exists():
        try:
            with open(cache_file, 'r') as f:
                return json.load(f)
        except:
            pass
    else:
        try:
            async def fetch_remote_config():
                async with httpx.AsyncClient(timeout=30.0) as client:
                    response = await client.post(
                        "https://mt.racers.co.kr/config",
                        json={"account_id": 0, "version": "0"},
                        headers={"Content-Type": "application/json"}
                    )
                    if response.status_code == 200:
                        data = response.json()
                        if "configs" in data:
                            TEMPER_CACHE_PATH.mkdir(exist_ok=True)
                            with open(cache_file, 'w') as f:
                                json.dump(data["configs"], f, indent=2)
                            return data["configs"]
            result = asyncio.run(fetch_remote_config())
            if result:
                return result
        except Exception as e:
            print(f"Failed to fetch remote config: {e}")
    return {}

def save_cached_config(config_data):
    """Save config modifications to cache"""
    TEMPER_CACHE_PATH.mkdir(exist_ok=True)
    cache_file = TEMPER_CACHE_PATH / "config_cache.json"
    with open(cache_file, 'w') as f:
        json.dump(config_data, f, indent=2)

def inject_config_ui(html_content):
    """Inject configuration UI into HTML"""
    try:
        with open('inject_ui.html', 'r', encoding='utf-8') as f:
            config_ui = f.read()
    except Exception as e:
        config_ui = f'<div style="color: red;">UI injection error: {e}</div>'

    if '</body>' in html_content:
        return html_content.replace('</body>', config_ui + '</body>')
    else:
        return html_content + config_ui

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

    for template_key, template in templates.items():
        if '/*' in template_key:
            template_parts = template_key.split(' ', 1)
            if len(template_parts) == 2:
                template_method, template_path_pattern = template_parts

                if template_method == method:
                    pattern = re.escape(template_path_pattern).replace(r'\*', '.*')
                    try:
                        if re.match(f"^{pattern}$", path):
                            return template_key, template
                    except re.error:
                        continue

    return None, None

def create_server_app(server_type="host"):
    try:
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
                full_path = request.url.path
                if request.url.query:
                    full_path += f"?{request.url.query}"
                template_key, template = find_template(templates, request.method, full_path)

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
                    try:
                        print(f"[{timestamp}] Body: {request_body.decode('utf-8', errors='ignore')}")
                    except:
                        print(f"[{timestamp}] Body: {request_body}")

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

                    async with httpx.AsyncClient(timeout=30.0) as client:
                        real_response = await client.request(
                            method=request.method,
                            url=url,
                            headers=headers,
                            content=request_body
                        )

                    response_content = real_response.content
                    response_status = real_response.status_code
                    response_headers = dict(real_response.headers)

                    if request.method == "POST" and request.url.path == "/config" and not template:
                        try:
                            response_data = real_response.json()
                            if "configs" in response_data:
                                template_data = {
                                    f"{request.method} {request.url.path}": {
                                        "response": response_data,
                                        "status_code": real_response.status_code
                                    }
                                }

                                captured_template_path = TEMPER_PATH / "captured_config.json"
                                with open(captured_template_path, 'w') as f:
                                    json.dump(template_data, f, indent=2)

                                templates = load_templates()
                                template_key, template = find_template(templates, request.method, request.url.path)

                        except Exception as e:
                            print(f"Failed to capture config template: {e}")

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

            if template and "request_body" in template:
                try:
                    template_request_body = template["request_body"]
                    if isinstance(template_request_body, dict):
                        if request_body:
                            try:
                                existing_body = json.loads(request_body.decode('utf-8'))
                                existing_body.update(template_request_body)
                                request_body = json.dumps(existing_body).encode('utf-8')
                            except:
                                request_body = json.dumps(template_request_body).encode('utf-8')
                        else:
                            request_body = json.dumps(template_request_body).encode('utf-8')
                    elif isinstance(template_request_body, str):
                        request_body = template_request_body.encode('utf-8')
                    if config.get("intercept_log", False):
                        timestamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                        print(f"[{timestamp}] Request body tempered: {request_body.decode('utf-8', errors='ignore')}")
                except Exception as e:
                    print(f"Request body tempering error: {e}")

            if template:
                template_response = template["response"]
                template_status = template.get("status_code", 200)

                if request.method == "POST" and request.url.path == "/config" and "configs" in template_response:
                    cached_config = load_cached_config()
                    if cached_config:
                        merged_configs = template_response["configs"].copy()
                        merged_configs.update(cached_config)
                        template_response = {"configs": merged_configs}

                if template.get("inject_ui", False) and response_content:
                    try:
                        html_str = response_content.decode('utf-8', errors='ignore')
                        modified_html = inject_config_ui(html_str)
                        response_content = modified_html.encode('utf-8')

                        modified_headers = dict(response_headers)
                        modified_headers.pop('content-length', None)
                        modified_headers.pop('Content-Length', None)

                        return Response(
                            content=response_content,
                            status_code=template_status,
                            headers=modified_headers
                        )
                    except Exception as e:
                        print(f"HTML injection error: {e}")
                        pass

                if not template.get("inject_ui", False):
                    return JSONResponse(content=template_response, status_code=template_status)

            if request.url.path.startswith('/api/'):
                response = await call_next(request)
                return response

            return Response(
                content=response_content,
                status_code=response_status,
                headers=response_headers
            )

        @app.get("/api/get-config")
        async def get_config():
            templates = load_templates()
            cached_config = load_cached_config()

            base_config = {}
            if "POST /config" in templates:
                config_template = templates["POST /config"]
                if "response" in config_template and "configs" in config_template["response"]:
                    base_config = config_template["response"]["configs"].copy()

            base_config.update(cached_config)

            return {"configs": base_config}

        @app.post("/api/save-config")
        async def save_config(request: Request):
            try:
                data = await request.json()
                ui_configs = data.get("configs", {})

                templates = load_templates()
                template_configs = {}
                if "POST /config" in templates:
                    config_template = templates["POST /config"]
                    if "response" in config_template and "configs" in config_template["response"]:
                        template_configs = config_template["response"]["configs"]

                user_modifications = {}
                for key, value in ui_configs.items():
                    template_value = template_configs.get(key)
                    if template_value is not None:
                        template_value_str = str(template_value)
                        ui_value_str = str(value)
                        if template_value_str != ui_value_str:
                            user_modifications[key] = value
                    else:
                        user_modifications[key] = value

                save_cached_config(user_modifications)
                return {"status": "saved", "message": "Configuration saved! Changes applied immediately."}
            except Exception as e:
                    return {"status": "error", "message": str(e)}

        @app.post("/api/reset-config")
        async def reset_config():
            try:
                TEMPER_CACHE_PATH.mkdir(exist_ok=True)
                cache_file = TEMPER_CACHE_PATH / "config_cache.json"
                with open(cache_file, 'w') as f:
                    json.dump({}, f)

                return {"status": "reset", "message": "Configuration reset to template defaults."}
            except Exception as e:
                return {"status": "error", "message": str(e)}

        @app.get("/api/list-templates")
        async def list_templates():
            """List available user templates"""
            try:
                templates_dir = TEMPER_PATH / "templates"
                templates_dir.mkdir(exist_ok=True)

                templates = []
                for template_file in templates_dir.glob("*.json"):
                    try:
                        with open(template_file, 'r') as f:
                            data = json.load(f)
                            templates.append({
                                "name": template_file.stem,
                                "file": template_file.name,
                                "title": data.get("title", template_file.stem),
                                "description": data.get("description", ""),
                                "created": data.get("created", ""),
                                "variables": len(data.get("configs", {}))
                            })
                    except:
                        pass

                return {"templates": templates}
            except Exception as e:
                return {"status": "error", "message": str(e)}

        @app.post("/api/save-template")
        async def save_template(request: Request):
            """Save current configuration as a template"""
            try:
                data = await request.json()
                template_name = data.get("name", "").strip()
                template_title = data.get("title", "").strip()
                description = data.get("description", "").strip()

                if not template_name:
                    return {"status": "error", "message": "Template name is required"}

                if not template_title:
                    return {"status": "error", "message": "Template title is required"}

                get_config_response = await get_config()
                current_configs = get_config_response.get("configs", {})

                template_data = {
                    "title": template_title,
                    "description": description,
                    "created": datetime.datetime.now().isoformat(),
                    "configs": current_configs
                }

                templates_dir = TEMPER_PATH / "templates"
                templates_dir.mkdir(exist_ok=True)

                template_file = templates_dir / f"{template_name}.json"
                with open(template_file, 'w') as f:
                    json.dump(template_data, f, indent=2)

                return {"status": "saved", "message": f"Template '{template_title}' saved successfully!"}
            except Exception as e:
                return {"status": "error", "message": str(e)}

        @app.post("/api/load-template")
        async def load_template(request: Request):
            """Load a template and apply its configuration"""
            try:
                data = await request.json()
                template_name = data.get("name", "").strip()

                if not template_name:
                    return {"status": "error", "message": "Template name is required"}

                templates_dir = TEMPER_PATH / "templates"
                template_file = templates_dir / f"{template_name}.json"

                if not template_file.exists():
                    return {"status": "error", "message": "Template not found"}

                with open(template_file, 'r') as f:
                    template_data = json.load(f)

                template_configs = template_data.get("configs", {})

                save_cached_config(template_configs)

                return {"status": "loaded", "message": f"Template '{template_name}' loaded successfully!", "configs": template_configs}
            except Exception as e:
                return {"status": "error", "message": str(e)}

        @app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"])
        async def catch_all(path: str = ""):
            """Fallback route"""
            return JSONResponse(content={"status": "ok"})

        print(f"App setup complete for {server_type}")
        return app
    except Exception as e:
        print(f"ERROR in create_server_app for {server_type}: {e}")
        import traceback
        traceback.print_exc()
        return None

def run_server(server_type, port):
    """Run a server of the specified type on the given port"""
    app = create_server_app(server_type)
    if app is None:
        print(f"ERROR: create_server_app returned None for {server_type}")
        return

    uvicorn.run(app, host="0.0.0.0", port=port, log_level="error")

if __name__ == '__main__':
    import threading

    host_port = int(sys.argv[1]) if len(sys.argv) > 1 else 81
    web_port = int(sys.argv[2]) if len(sys.argv) > 2 else 82

    config = load_config()
    templates = load_templates()

    print("🚀 Starting Dual Server Setup")
    print(f"📝 Logging: {'enabled' if config.get('intercept_log') else 'disabled'}")
    print(f"🔄 Passthrough: {'enabled' if config.get('passthrough') else 'disabled'}")
    print(f"🎯 Templates: {'enabled' if config.get('temper') else 'disabled'}")
    print(f"📋 Templates loaded: {len(templates)}")
    print("Press Ctrl+C to stop both servers")
    print()
    host_thread = threading.Thread(
        target=run_server,
        args=("host", host_port),
        daemon=True
    )
    host_thread.start()

    web_thread = threading.Thread(
        target=run_server,
        args=("web", web_port),
        daemon=True
    )
    web_thread.start()

    print("✅ Both servers started!")
    print(f"🌐 Host Server (mh.hostServerUrl): http://localhost:{host_port} -> https://mt.racers.co.kr/")
    print(f"🌐 Web Server (mh.webServerUrl): http://localhost:{web_port} -> https://www.racers.co.kr")
    print("Press Ctrl+C to stop both servers")
    print()

    try:
        while True:
            import time
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n🛑 Shutting down servers...")
        exit(0)
