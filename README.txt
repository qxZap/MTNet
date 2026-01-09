MTNet - My Truck Town Game Modifier
==================================

MTNet lets you customize your My Truck Town gaming experience. You can change money earnings, experience points, and many other game settings while you play.

WHAT YOU CAN DO
===============

Real-Time Configuration Editor
------------------------------
- Change any game setting instantly while playing
- Add your own custom settings
- Settings take effect immediately without restarting the game

Economy and Rewards
-------------------
- Change how much money you earn from different jobs:
  - Bus services
  - Taxi fares
  - Limousine rides
  - Tow truck jobs
  - Police work
- Adjust experience points for driving and drifting
- Modify population bonuses

Pre-Made Templates
------------------
Ready-to-use setups for common changes:
- Economy multipliers (2x, 4x, 5x, 10x, 20x, 100x earnings)
- Special character setups
- Enhanced vehicle features

Custom Templates
---------------
- Save your favorite settings as templates
- Load complete setups with one click
- Mix and match different settings

INSTALLATION
============

First off, have Python installed and run server.bat before starting the game.

Add MTNet_P.pak in the Paks folder and then start the game.

Menu will appear in game.

HOW TO USE
==========

1. Start MTNet by double-clicking server.bat
2. Put the .pak mod file in your game's Paks folder
3. Start My Truck Town
4. Look for the gear icon in the top-right corner
5. Click it to open the settings menu

In the menu you can:
- Change any setting you want
- Load pre-made templates
- Save your own custom setups
- Add new settings using the "Add Setting" button

TROUBLESHOOTING
===============

Game won't start:
- Make sure MTNet server is running
- Check that the .pak file is in the correct Paks folder
- Make sure ports 81 and 82 aren't blocked

Menu not showing:
- Verify the game mod is installed correctly
- Try pressing F5 to refresh the game page

Settings not working:
- Click "Save All Changes" after making changes
- Try the "Reload" button

Performance problems:
- Enable request silencing in templates
- Make sure you have a good internet connection

TECHNICAL DETAILS
=================

For advanced users and developers:

Server Configuration (server_config.json):
- intercept_log: Turn detailed logging on/off
- passthrough: Send requests to real game servers
- temper: Use templates to modify responses

Common Game Variables:
- mh.busPaymentMultiplier: Bus earnings
- mh.taxiPaymentMultiplier: Taxi earnings
- mh.limoPaymentMultiplier: Limousine earnings
- mh.towPaymentMultiplier: Tow truck earnings
- mh.policePatrolPayment: Police pay
- mh.driverXPMultiplierOnAutopilot: XP while using autopilot
- mh.vehicleDriftExpPerSeconds: XP from drifting
- mh.invalidPartsDisableEngine: What happens with broken parts
- mh.logFPSPeriodSeconds: Performance logging

Template System:
Templates are stored in the temper/ folder as JSON files.
You can create custom templates using the in-game menu or by editing JSON files directly.

Request Silencing:
Use silenced_*.json templates to block unwanted data from being sent to game servers.

File Structure:
- server.py: Main server program
- server_config.json: Server settings
- requirements.txt: Python packages needed
- server.bat: Installs dependencies and starts the server
- inject_ui.html: The in-game menu interface
- temper/: Template storage
  - game_config.json: Basic game settings
  - motortown_ui.json: Menu injection
  - templates/: Your saved templates
  - silenced_*.json: Data blocking templates
- temper_cache/: Saved configurations

Safety Notice:
MTNet only affects your local game. It doesn't change the game for other players or modify game servers. Use responsibly and follow the game's terms of service.
