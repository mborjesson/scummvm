/* ScummVM - Graphic Adventure Engine
*
* ScummVM is the legal property of its developers, whose names
* are too numerous to list here. Please refer to the COPYRIGHT
* file distributed with this source distribution.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/

#include "backends/keymapper/keymapper.h"

#ifdef ENABLE_KEYMAPPER

#include "common/config-manager.h"

namespace Common {

void Keymapper::Domain::addKeymap(Keymap *map) {
	iterator it = find(map->getName());

	if (it != end())
		delete it->_value;

	setVal(map->getName(), map);
}

void Keymapper::Domain::deleteAllKeyMaps() {
	for (iterator it = begin(); it != end(); ++it)
		delete it->_value;

	clear();
}

Keymap *Keymapper::Domain::getKeymap(const String& name) {
	iterator it = find(name);

	if (it != end())
		return it->_value;
	else
		return 0;
}

Keymapper::Keymapper(EventManager *evtMgr)
	: _eventMan(evtMgr), _enabled(true), _hardwareInputs(0) {
	ConfigManager::Domain *confDom = ConfMan.getDomain(ConfigManager::kKeymapperDomain);

	_globalDomain.setConfigDomain(confDom);
}

Keymapper::~Keymapper() {
	delete _hardwareInputs;
}

void Keymapper::registerHardwareInputSet(HardwareInputSet *inputs) {
	if (_hardwareInputs)
		error("Hardware input set already registered");

	if (!inputs) {
		warning("No hardware input were defined, using defaults");
		inputs = new HardwareInputSet(true);
	}

	_hardwareInputs = inputs;
}

void Keymapper::addGlobalKeymap(Keymap *keymap) {
	initKeymap(_globalDomain, keymap);
}

void Keymapper::addGameKeymap(Keymap *keymap) {
	if (ConfMan.getActiveDomain() == 0)
		error("Call to Keymapper::addGameKeymap when no game loaded");

	// Detect whether the active game changed since last call.
	// If so, flush the game key configuration.
	if (_gameDomain.getConfigDomain() != ConfMan.getActiveDomain()) {
		cleanupGameKeymaps();
		_gameDomain.setConfigDomain(ConfMan.getActiveDomain());
	}

	initKeymap(_gameDomain, keymap);
}

void Keymapper::initKeymap(Domain &domain, Keymap *map) {
	if (!_hardwareInputs) {
		warning("No hardware inputs were registered yet (%s)", map->getName().c_str());
		return;
	}

	map->setConfigDomain(domain.getConfigDomain());
	map->loadMappings(_hardwareInputs);

	if (map->isComplete(_hardwareInputs) == false) {
		map->saveMappings();
		ConfMan.flushToDisk();
	}

	domain.addKeymap(map);
}

void Keymapper::cleanupGameKeymaps() {
	// Flush all game specific keymaps
	_gameDomain.deleteAllKeyMaps();

	// Now restore the stack of active maps. Re-add all global keymaps, drop
	// the game specific (=deleted) ones.
	Stack<MapRecord> newStack;

	for (Stack<MapRecord>::size_type i = 0; i < _activeMaps.size(); i++) {
		if (_activeMaps[i].global)
			newStack.push(_activeMaps[i]);
	}

	_activeMaps = newStack;
}

Keymap *Keymapper::getKeymap(const String& name, bool *globalReturn) {
	Keymap *keymap = _gameDomain.getKeymap(name);
	bool global = false;

	if (!keymap) {
		keymap = _globalDomain.getKeymap(name);
		global = true;
	}

	if (globalReturn)
		*globalReturn = global;

	return keymap;
}

bool Keymapper::pushKeymap(const String& name, bool transparent) {
	bool global;

	assert(!name.empty());
	Keymap *newMap = getKeymap(name, &global);

	if (!newMap) {
		warning("Keymap '%s' not registered", name.c_str());
		return false;
	}

	pushKeymap(newMap, transparent, global);

	return true;
}

void Keymapper::pushKeymap(Keymap *newMap, bool transparent, bool global) {
	MapRecord mr = {newMap, transparent, global};

	_activeMaps.push(mr);
}

void Keymapper::popKeymap(const char *name) {
	if (!_activeMaps.empty()) {
		if (name) {
			String topKeymapName = _activeMaps.top().keymap->getName();
			if (topKeymapName.equals(name))
				_activeMaps.pop();
			else
				warning("An attempt to pop wrong keymap was blocked (expected %s but was %s)", name, topKeymapName.c_str());
		} else {
			_activeMaps.pop();
		}
	}

}

List<Event> Keymapper::mapEvent(const Event &ev, EventSource *source) {
	if (source && !source->allowMapping()) {
		return DefaultEventMapper::mapEvent(ev, source);
	}

	List<Event> mappedEvents;

	if (ev.type == Common::EVENT_KEYDOWN)
		mappedEvents = mapKeyDown(ev.kbd);
	else if (ev.type == Common::EVENT_KEYUP)
		mappedEvents = mapKeyUp(ev.kbd);

	if (!mappedEvents.empty())
		return mappedEvents;
	else
		return DefaultEventMapper::mapEvent(ev, source);
}

List<Event> Keymapper::mapKeyDown(const KeyState& key) {
	return mapKey(key, true);
}

List<Event> Keymapper::mapKeyUp(const KeyState& key) {
	return mapKey(key, false);
}

List<Event> Keymapper::mapKey(const KeyState& key, bool keyDown) {
	if (!_enabled || _activeMaps.empty())
		return List<Event>();

	Action *action = 0;

	if (keyDown) {
		// Search for key in active keymap stack
		for (int i = _activeMaps.size() - 1; i >= 0; --i) {
			MapRecord mr = _activeMaps[i];
			debug(5, "Keymapper::mapKey keymap: %s", mr.keymap->getName().c_str());
			action = mr.keymap->getMappedAction(key);

			if (action || !mr.transparent)
				break;
		}

		if (action)
			_keysDown[key] = action;
	} else {
		HashMap<KeyState, Action *>::iterator it = _keysDown.find(key);

		if (it != _keysDown.end()) {
			action = it->_value;
			_keysDown.erase(key);
		}
	}

	if (!action)
		return List<Event>();

	return executeAction(action, keyDown);
}

Action *Keymapper::getAction(const KeyState& key) {
	Action *action = 0;

	return action;
}

List<Event> Keymapper::executeAction(const Action *action, bool keyDown) {
	List<Event> mappedEvents;
	List<Event>::const_iterator it;

	for (it = action->events.begin(); it != action->events.end(); ++it) {
		Event evt = *it;

		switch (evt.type) {
		case EVENT_KEYDOWN:
			if (!keyDown) evt.type = EVENT_KEYUP;
			break;
		case EVENT_KEYUP:
			if (keyDown) evt.type = EVENT_KEYDOWN;
			break;
		case EVENT_LBUTTONDOWN:
			if (!keyDown) evt.type = EVENT_LBUTTONUP;
			break;
		case EVENT_LBUTTONUP:
			if (keyDown) evt.type = EVENT_LBUTTONDOWN;
			break;
		case EVENT_RBUTTONDOWN:
			if (!keyDown) evt.type = EVENT_RBUTTONUP;
			break;
		case EVENT_RBUTTONUP:
			if (keyDown) evt.type = EVENT_RBUTTONDOWN;
			break;
		case EVENT_MBUTTONDOWN:
			if (!keyDown) evt.type = EVENT_MBUTTONUP;
			break;
		case EVENT_MBUTTONUP:
			if (keyDown) evt.type = EVENT_MBUTTONDOWN;
			break;
		case EVENT_MAINMENU:
			if (!keyDown) evt.type = EVENT_MAINMENU;
			break;
		default:
			// don't deliver other events on key up
			if (!keyDown) continue;
		}

		evt.mouse = _eventMan->getMousePos();
		mappedEvents.push_back(evt);
	}
	return mappedEvents;
}

const HardwareInput *Keymapper::findHardwareInput(const KeyState& key) {
	return (_hardwareInputs) ? _hardwareInputs->findHardwareInput(key) : 0;
}

} // End of namespace Common

#endif // #ifdef ENABLE_KEYMAPPER
