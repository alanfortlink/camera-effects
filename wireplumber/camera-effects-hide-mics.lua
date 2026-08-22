-- Camera Effects — hide the real microphones from apps.
--
-- Installed and enabled by the panel's "Hide raw microphone(s) from apps"
-- switch (see scripts/camera-effects-mic-hide), never by default. It takes the
-- same permission route WirePlumber's own software-dsp script uses to hide the
-- hardware node behind a filter: every client except WirePlumber itself and
-- Camera Effects loses the right to see the listed Audio/Source nodes, so the
-- only microphone left in their lists is "Microphone Effects" — while the
-- daemon can still read the real one.
--
-- Which nodes are hidden comes from the conf.d fragment that loads this
-- script:
--   camera-effects.hide-mics = { all = true }               all microphones
--   camera-effects.hide-mics = { nodes = [ "node.name" ] }  these ones
--
-- This is a courtesy boundary for well-behaved apps, exactly like the camera
-- side: anything that talks to PipeWire as its own client with its own name
-- can still ask for the raw device.

local log = Log.open_topic ("s-camera-effects")

-- This is loaded as a *required* component (the only flag WirePlumber honours
-- for a custom one — see camera-effects-mic-hide), which means an error raised
-- at load time would stop WirePlumber from starting and take every audio
-- device with it. So nothing here may raise: the configuration is read inside
-- a pcall and anything unexpected simply means "hide nothing".
--
-- parse(2): the node list is one level below the object, and a shallower
-- parse would hand back the array as a Json object instead of a table.
local hide_all = false
local hide_names = {}

local ok, err = pcall (function ()
  local cfg = Conf.get_section_as_json ("camera-effects.hide-mics", Json.Object {}):parse (2)
  if type (cfg) ~= "table" then return end
  hide_all = cfg ["all"] == true
  local names = cfg ["nodes"]
  if names ~= nil and type (names) ~= "table" then names = names:parse () end
  if type (names) == "table" then
    for _, n in ipairs (names) do hide_names [tostring (n)] = true end
  end
end)
if not ok then
  log:info ("hide-mics: unreadable configuration, hiding nothing (" .. tostring (err) .. ")")
  hide_all = false
  hide_names = {}
end
log:info ("hide-mics: all=" .. tostring (hide_all))

local hidden = {}      -- bound-id -> node.name
-- Hiding only ever applies while the replacement exists. WirePlumber runs from
-- login; the daemon that publishes "camera-effects-mic" only starts with the
-- shell, and it can crash or be uninstalled without this file going with it.
-- Withholding the permission until our node is actually there turns "no
-- microphone at all" into "the real ones are visible for a few seconds".
local ours = false

local clients_om = ObjectManager { Interest { type = "client" } }

local function should_hide (node)
  local props = node.properties
  local class = props ["media.class"] or ""
  if class ~= "Audio/Source" and class ~= "Audio/Source/Virtual" then return false end
  local name = props ["node.name"] or ""
  if name:sub (1, 14) == "camera-effects" then return false end   -- our own virtual mic
  if hide_all then return true end
  return hide_names [name] == true
end

-- WirePlumber must keep full access (it does the linking), and so must the
-- Camera Effects daemon (it reads the microphone it hides).
local function allowed (client)
  local props = client.properties
  if props ["wireplumber.daemon"] then return true end
  if props ["camera-effects.client"] == "true" then return true end
  return false
end

local function apply (client)
  if allowed (client) then return end
  local perms = {}
  local any = false
  for id, _ in pairs (hidden) do perms [id] = ours and "-" or "rwxm"; any = true end
  if any then client:update_permissions (perms) end
end

local function apply_all ()
  for client in clients_om:iterate { type = "client" } do apply (client) end
end

-- The node-added *event* (rather than the object manager's signal) is what
-- WirePlumber's own software-dsp script hooks to hide a hardware node: by then
-- the node is bound and has the global id the permission refers to.
SimpleEventHook {
  name = "camera-effects/hide-mics",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
    },
  },
  execute = function (event)
    local node = event:get_subject ()
    local name = node.properties ["node.name"] or ""
    if not should_hide (node) then return end
    local ok, id = pcall (function () return node ["bound-id"] end)
    if not ok or id == nil then return end
    hidden [id] = name
    log:info ("hiding microphone " .. tostring (name))
    apply_all ()
  end
}:register ()

-- Whether "Microphone Effects" exists is checked once a second against an
-- object manager. The obvious routes do not work: a node on its way out is
-- already unbound (the node-removed event carries no usable identity, and
-- asking only logs a warning), and the object manager's own object-removed
-- signal was measured never to fire for this node. A one-second poll of a
-- lookup is dull and it is right.
local nodes_om = ObjectManager { Interest { type = "node" } }
nodes_om:activate ()

local function check_ours ()
  local found = nodes_om:lookup {
    Constraint { "node.name", "=", "camera-effects-mic", type = "pw-global" },
  } ~= nil
  if found == ours then return end
  ours = found
  log:info (ours and "hide-mics: Microphone Effects is up, hiding the real microphones"
                  or "hide-mics: Microphone Effects is gone, unhiding the real microphones")
  apply_all ()
end
Core.timeout_add (1000, function ()
  check_ours ()
  return true   -- keep repeating
end)

-- A client that connects later must not see them either.
clients_om:connect ("object-added", function (om, client) apply (client) end)
clients_om:activate ()
