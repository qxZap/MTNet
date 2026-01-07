INTERCEPTING PROXY SERVER
=========================

This is an intercepting proxy server for mt.racers.co.kr that can:
- Log incoming requests (configurable)
- Forward requests to the real server (passthrough mode)
- Modify responses using templates (temper mode)

REQUIREMENTS
============
- Python 3.8 or higher must be installed
- Internet connection for passthrough mode

INSTALLATION
============
1. Run install.bat to install required Python packages
2. Configure server_config.json and map.json as needed

CONFIGURATION
=============
server_config.json:
- intercept_log: true/false - Enable/disable request logging
- passthrough: true/false - Forward requests to real servers (or block them)
- temper: true/false - Apply response templates to override responses

TEMPLATES
=========
Templates are stored in the 'temper/' folder as individual JSON files.
Each file can contain one or more template definitions.

Example template file (game_config.json):
{
  "POST /config": {
    "response": {"configs": {...}},
    "status_code": 200
  }
}

To silence requests (no logging, no passthrough):
{
  "POST /log": {
    "silence": true
  }
}

RESPONSE FLOW:
==============
1. If passthrough=true: Forward request to real server and get response
2. If passthrough=false: Return default {"status": "ok"} response
3. If temper=true and template exists: Override response with template
4. Return final response

map.json:
- Contains response templates for specific endpoints
- Format: {"METHOD /path": {"response": {...}, "status_code": 200}}

USAGE
=====
1. Run install.bat (first time only)
2. Run server.bat to start both proxy servers
3. Configure your application:
   - mh.hostServerUrl="http://localhost:81"
   - mh.webServerUrl="http://localhost:82"
4. Both servers will intercept and forward requests based on configuration

EXAMPLES
========
- Default config forwards all requests to mt.racers.co.kr with logging disabled
- POST /config returns modified game configuration instead of real response
- Templates can override any endpoint response

TROUBLESHOOTING
===============
- Make sure Python is in your PATH
- Check that port 8081 is not in use by other applications
- Verify server_config.json and map.json are valid JSON files
- Enable intercept_log in server_config.json for debugging

FILES
=====
- mock_server.py: Main proxy server
- server_config.json: Configuration settings
- map.json: Response templates
- install.bat: Install dependencies
- server.bat: Start the server
- requirements.txt: Python dependencies
