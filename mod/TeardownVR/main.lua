-- ===========================================================================
-- Teardown VR
-- Pilote la caméra, la visée, l'outil et les déplacements en réalité virtuelle.
--
-- Ce script parle avec la DLL "dsound.dll" (Teardown VR) installée dans le
-- dossier du jeu. Le canal passe par SetClipboardText / GetClipboardText :
-- la DLL intercepte ces messages (préfixe TDVR2), ils n'arrivent jamais dans
-- le vrai presse-papiers de Windows.
-- ===========================================================================

local PROTO = "TDVR2"   -- (TDVR1 : les anciennes copies du mod, que la DLL laisse inactives)
local camDebug = false

local seq = 0
local wasActive = false
local vrYaw = 0             -- rotation de la "pièce" VR dans le monde (degrés)
local lastRecenter = -1
local snapReady = true
local hapL, hapR = 0, 0
local vignette = 0
local lastTool = ""
local debugInfo = false
local debugMsg = nil      -- texte envoyé au journal de la DLL (diagnostic)
local syncTest = false
local syncEye = 0
local fovTestLogged = false
local hudTest = { fov = false, modCam = false, prevClick = false }   -- essais du HUD (voir tick et draw)
local modCamNear = false   -- la caméra du jeu est aux yeux du joueur (voir tick)
local frozenFrame = false
local lastCam = nil
local lastEyeRoom = nil   -- pose de l'œil dans la pièce (repère du casque)
local lastScale = 1
local lastCamEye = 0
local camErr = { 0, 0, 0, 0, 0, 0 } -- par œil : max écart position, angle, nb d'images fausses
local camErrN = 0
local camBadLogs = 0
local camBadDetail = nil
local stereoMsg = nil
local aimMsg = nil
local lastTickDt = 0
local tickCount, drawCount, drawTick = 0, 0, -1   -- (drawTick : tickCount au dernier draw)
-- Outil physique (voir plus bas) : la copie de l'outil, un vrai objet du jeu.
local physMsg = nil   -- message de diagnostic de l'outil physique (prioritaire)
local lastVehYaw = nil   -- lacet du véhicule à l'image précédente (la vue tourne avec lui)
local physShowReal = false   -- diagnostic : montre aussi l'outil du jeu, à la main (mesure du retard)
local phys = { body = 0, key = nil, offset = nil, want = nil, prevWant = nil, hidden = {}, stuckT = 0, mass = 0, logT = 0 }
local physTool, physDestroy, physSetPlayerMask
-- Bras de la main libre (voir tdvrOffArmTick, plus bas) : corps de la main et de l'avant-bras.
local offArm = { hand = 0, arm = 0, shapes = {}, side = nil, vox = nil, shown = false, sendT = 0, checkT = 0, waitT = 0 }
-- Main libre physique, comme Bonelabs (voir tdvrOffHandTick, plus bas) : son corps, l'objet qu'elle tient.
local offHand = { body = 0, shape = 0, held = 0, press = 0, waitT = 0, reachN = 0, reachSent = false }
-- Chemins rapides (réglage [Performance] fastLua de la DLL, bit 64 de debugFlags, relu à chaque
-- requête) : mêmes résultats avec moins d'appels au moteur et moins de tables créées. Coupé
-- (fastLua=0, ou une DLL plus ancienne) : le code d'avant, tel quel. Voir request, tick (toWorld),
-- physTool, tdvrOffHandTick (offHand.find), offHand.restore, updateInventory.
--   on : réglage de la dernière requête complète ; tagsOk : étiquettes de la réponse indexées
--   pour les jetons actuels (tags : étiquette -> indice) ; tc : formes de l'outil en main.
local fastLua = { on = false, tagsOk = false, tags = {}, tc = { list = {}, age = 0 } }

-- Rejette la copie physique de l'outil de la prochaine requête (rayons, sphères).
local function rejectPhys()
	if phys.body ~= 0 then QueryRejectBody(phys.body) end
	if offArm.hand ~= 0 then QueryRejectBody(offArm.hand) end   -- (le bras de la main libre non plus)
	if offArm.arm ~= 0 then QueryRejectBody(offArm.arm) end
	if offHand.body ~= 0 then QueryRejectBody(offHand.body) end   -- (ni la main physique)
	if tdvrDual and tdvrDual.copy ~= 0 then QueryRejectBody(tdvrDual.copy) end   -- (ni l'arme de l'autre main, voir tdvrDualTick)
end

local function num(t, i) return tonumber(t[i]) or 0 end

local function pose(t, i, fast)
	if fast then
		-- (fastLua : la même pose en tables simples, sans 3 appels au moteur ; comme Transform,
		-- quaternion normalisé, et nul -> identité)
		local x, y, z, w = num(t, i + 3), num(t, i + 4), num(t, i + 5), num(t, i + 6)
		local l = math.sqrt(x * x + y * y + z * z + w * w)
		if l > 0 then
			x, y, z, w = x / l, y / l, z / l, w / l
		else
			x, y, z, w = 0, 0, 0, 1
		end
		return { pos = { num(t, i), num(t, i + 1), num(t, i + 2) }, rot = { x, y, z, w } }
	end
	return Transform(Vec(num(t, i), num(t, i + 1), num(t, i + 2)),
		Quat(num(t, i + 3), num(t, i + 4), num(t, i + 5), num(t, i + 6)))
end

local function clamp(v, lo, hi)
	if v < lo then return lo end
	if v > hi then return hi end
	return v
end

local function quatConj(q) return Quat(-q[1], -q[2], -q[3], q[4]) end

-- Rotation (quaternion) -> vecteur rotation (axe x angle, radians), chemin le plus court.
local function quatToRotVec(q)
	local x, y, z, w = q[1], q[2], q[3], q[4]
	if w < 0 then x, y, z, w = -x, -y, -z, -w end
	local s = math.sqrt(x * x + y * y + z * z)
	if s < 1e-6 then return Vec(2 * x, 2 * y, 2 * z) end
	local a = 2 * math.atan2(s, w)
	return Vec(x / s * a, y / s * a, z / s * a)
end

-- Canal avec la DLL. Si la DLL ne répond pas (pas installée, ou mod installé
-- au mauvais endroit), on réessaie seulement toutes les 2 secondes.
local failures = 0
local exchangeLogs = 0
local pendingExchangeLog = nil
local retryAt = 0
local warned = false

-- Attention : SetClipboardText du jeu relit le texte après l'avoir libéré
-- (bug du jeu). Un texte de 30 caractères au plus reste dans la chaîne
-- elle-même et ne craint rien : les requêtes restent donc courtes.
local function exchange(msg)
	local ok, s
	for attempt = 1, 2 do
		-- Appels directs (pas de fonction créée à chaque image : moins de déchets mémoire).
		ok = pcall(SetClipboardText, msg)
		if ok then ok, s = pcall(GetClipboardText) end
		if ok and type(s) == "string" and string.sub(s, 1, 5) == PROTO then
			failures = 0
			return s
		end
	end
	failures = failures + 1
	if camDebug and exchangeLogs < 10 then
		exchangeLogs = exchangeLogs + 1
		pendingExchangeLog = string.format("exchange failed ok=%s type=%s val=%s", tostring(ok), type(s),
			string.sub(tostring(s), 1, 60))
	end
	if failures > 30 then
		retryAt = GetTime() + 2
		if not warned then
			warned = true
			DebugPrint("Teardown VR: no response from dsound.dll. Check that the mod is in the game's 'mods' folder (see README). / Teardown VR : pas de réponse de dsound.dll. Vérifie que le mod est dans le dossier 'mods' du jeu (voir LISEZMOI).")
		end
	end
	return nil
end

-- ---------------------------------------------------------------------------
-- Inventaire VR : les armes et les outils (mods compris), affichés par la DLL
-- dans le casque comme la barre d'outils du jeu sur PC. Seul ce script peut lire
-- la liste des outils (registre "game.tool.*") : il l'envoie à la DLL en
-- messages courts (30 caractères au plus, voir exchange), seulement quand elle
-- change :
--   TDVRIB <ver> <nombre>                        début de la liste <ver> (1..99)
--   TDVRIT <ver> <i> <groupe> <morceaux> <dispo> <munitions>
--   TDVRIN <ver> <i> <k> <texte>                 k-ième morceau du nom de l'outil i
--   TDVRIA <ver> <i> <dispo> <munitions>         munitions / disponibilité changées
--   TDVRIC <ver> <i> <bloqué>                    outil en main (0 = aucun)
-- Les textes passent en ASCII : les accents (UTF-8) et "~" deviennent "~XX"
-- (hexadécimal) ; "~" seul = texte vide. La réponse de la DLL finit par
-- "INV <liste reçue> <choix n°> <choix liste> <choix i> <ouvert>" : l'outil
-- choisi dans le casque est pris ici, sur une image normale.
-- ---------------------------------------------------------------------------
local INV_BUDGET = 6   -- messages au plus par image (une liste de 40 outils part en ~15 images)
local inv = { ver = 0, sig = nil, ids = {}, msgs = {}, pos = 1, doneT = nil, dynSent = {}, queue = {},
	cur = nil, tool = nil, curIdx = 0, scanT = 0, dynT = 0, selSeq = nil, open = false, names = {} }
local invResp = { ok = false, ack = -1, seq = 0, ver = 0, idx = 0, open = 0 }

-- Champs "INV ..." de la réponse (repérés par leur étiquette, après les réglages).
-- Bouche du canon de l'outil d'un mod (« PIV x y z » de la réponse) : le point autour
-- duquel la DLL le réduit, dans le repère de son corps (non réduit). Renvoie aussi le
-- texte reçu, qui sert de clé (copie physique et collisions refaites s'il change).
-- (fastLua) Les étiquettes de la réponse (" PIV ", " ARM ", " LAS ", " DUA ", " INV ") repérées en
-- un seul passage au lieu de cinq ; fastLua.tag(nom, de, à) renvoie le même indice que la
-- boucle « for j = de, à do if jetons[j] == nom » (chaque étiquette est unique dans la réponse).
fastLua.names = { PIV = true, ARM = true, LAS = true, DUA = true, INV = true }
fastLua.index = function(t, n)
	local g, names = fastLua.tags, fastLua.names
	g.PIV, g.ARM, g.LAS, g.DUA, g.INV = nil, nil, nil, nil, nil
	for j = 80, n do
		local w = t[j]
		if names[w] and not g[w] then g[w] = j end
	end
end
fastLua.tag = function(name, from, last)
	local j = fastLua.tags[name]
	if j and j >= from and j <= last then return j end
	return nil
end

local function pivParse(t, n)
	if fastLua.tagsOk then
		local j = fastLua.tag("PIV", 100, n - 3)
		if j then
			return Vec(tonumber(t[j + 1]) or 0, tonumber(t[j + 2]) or 0, tonumber(t[j + 3]) or 0),
				t[j + 1] .. " " .. t[j + 2] .. " " .. t[j + 3]
		end
		return Vec(0, 0, 0), ""
	end
	for j = 100, n - 3 do
		if t[j] == "PIV" then
			return Vec(tonumber(t[j + 1]) or 0, tonumber(t[j + 2]) or 0, tonumber(t[j + 3]) or 0),
				t[j + 1] .. " " .. t[j + 2] .. " " .. t[j + 3]
		end
	end
	return Vec(0, 0, 0), ""
end

local function invParse(t, n)
	invResp.ok = false
	if fastLua.tagsOk then
		local j = fastLua.tag("INV", 80, n - 5)
		if j then
			invResp.ack = tonumber(t[j + 1]) or -1
			invResp.seq = tonumber(t[j + 2]) or 0
			invResp.ver = tonumber(t[j + 3]) or 0
			invResp.idx = tonumber(t[j + 4]) or 0
			invResp.open = tonumber(t[j + 5]) or 0
			invResp.ok = true
		end
		return
	end
	for j = 80, n - 5 do
		if t[j] == "INV" then
			invResp.ack = tonumber(t[j + 1]) or -1
			invResp.seq = tonumber(t[j + 2]) or 0
			invResp.ver = tonumber(t[j + 3]) or 0
			invResp.idx = tonumber(t[j + 4]) or 0
			invResp.open = tonumber(t[j + 5]) or 0
			invResp.ok = true
			return
		end
	end
end

-- Texte -> ASCII sûr pour le canal ("~XX" pour le reste) ; max : longueur maximale
-- (coupé entre deux lettres, jamais au milieu d'une lettre accentuée).
local function invEscape(s, max)
	local out, len, i = {}, 0, 1
	while i <= #s do
		local b = string.byte(s, i)
		local n = b >= 240 and 4 or b >= 224 and 3 or b >= 192 and 2 or 1   -- octets de la lettre (UTF-8)
		local c = string.sub(s, i, i + n - 1)
		if b < 32 or b > 125 then
			c = string.gsub(c, ".", function(x) return string.format("~%02X", string.byte(x)) end)
		end
		if max and len + #c > max then break end
		out[#out + 1] = c
		len, i = len + #c, i + n
	end
	return table.concat(out)
end

-- Coupe un texte UTF-8 à max octets sans couper une lettre accentuée, avec "…".
local function invCut(s, max)
	if #s <= max then return s end
	local n = max
	while n > 0 do
		local b = string.byte(s, n + 1)
		if b < 128 or b >= 192 then break end   -- l'octet suivant commence une lettre
		n = n - 1
	end
	return string.sub(s, 1, n) .. "\226\128\166"
end

-- Nom affiché d'un outil : la traduction du jeu (comme son HUD), sinon le texte brut.
local function invName(id, key)
	if key == "" then return id end
	local s = inv.names[key]
	if s then return s end
	local bare = string.gsub(key, "^loc@", "")
	s = bare
	for _, k in ipairs({ key, bare }) do
		local ok, has = pcall(HasTranslationByKey, k)
		if ok and has then
			local ok2, tr = pcall(GetTranslatedStringByKey, k)
			if ok2 and type(tr) == "string" and tr ~= "" then
				s = tr
				break
			end
		end
	end
	inv.names[key] = s
	return s
end

-- Disponibilité et munitions d'un outil, comme le HUD du jeu : "1 12", "0 0", "1 ~"...
local function invDyn(id)
	local p = "game.tool." .. id
	local sel = not HasKey(p .. ".selectable") or GetBool(p .. ".selectable")
	local ammo = GetInt(p .. ".ammo")
	local disp = ""
	if ammo < 9990 then   -- (bac à sable : 9999, rien d'affiché)
		if HasKey(p .. ".ammo.display") then
			disp = GetString(p .. ".ammo.display")
		elseif not (ammo == 0 and sel) then
			disp = tostring(ammo)
		end
	end
	disp = invEscape(disp, 10)
	return (sel and "1 " or "0 ") .. (disp == "" and "~" or disp)
end

-- Outils activés, dans l'ordre du jeu (groupe, puis rang dans le groupe).
-- (fastLua : job, budget - le même parcours étalé sur plusieurs images, budget outils par appel ;
-- renvoie nil et le job tant qu'il n'est pas fini. Sans eux : tout de suite, comme avant.)
local function invScan(job, budget)
	job = job or { keys = ListKeys("game.tool") or {}, i = 1, list = {} }
	local keys, list = job.keys, job.list
	local last = budget and math.min(#keys, job.i + budget - 1) or #keys
	for i = job.i, last do
		local id = keys[i]
		local p = "game.tool." .. id
		if GetBool(p .. ".enabled") and #list < 200 then
			local g = GetInt(p .. ".group")
			if g < 1 or g > 9 then g = 6 end   -- (outil de mod sans groupe : le 6, comme le jeu)
			list[#list + 1] = { id = id, g = g, index = GetInt(p .. ".index"), name = invName(id, GetString(p .. ".name")) }
		end
	end
	job.i = last + 1
	if job.i <= #keys then return nil, job end
	table.sort(list, function(a, b)
		if a.g ~= b.g then return a.g < b.g end
		if a.index ~= b.index then return a.index < b.index end
		return a.id < b.id
	end)
	return list
end

-- Munitions et disponibilité des outils from .. from + budget - 1 de la liste envoyée (tous, sans
-- budget) : ce qui a changé part à la DLL. Renvoie l'outil suivant, nil une fois la liste finie.
inv.dynPass = function(from, budget)
	local ids = inv.ids
	local last = budget and math.min(#ids, from + budget - 1) or #ids
	for i = from, last do
		local d = invDyn(ids[i])
		if d ~= inv.dynSent[i] then
			inv.dynSent[i] = d
			inv.queue[#inv.queue + 1] = string.format("TDVRIA %d %d %s", inv.ver, i, d)
		end
	end
	if last < #ids then return last + 1 end
	return nil
end

-- Liste changée : nouvelle version et messages d'un envoi complet.
local function invBuild(list)
	local parts = {}
	for i, e in ipairs(list) do parts[i] = e.id .. "\1" .. e.g .. "\1" .. e.name end
	local sig = table.concat(parts, "\2")
	if sig == inv.sig then return end
	inv.sig = sig
	inv.ver = inv.ver % 99 + 1
	local v = inv.ver
	local msgs, ids, sent = { string.format("TDVRIB %d %d", v, #list) }, {}, {}
	for i, e in ipairs(list) do
		ids[i] = e.id
		local name = invEscape(invCut(e.name, 38))
		local chunks = {}
		for k = 1, #name, 14 do chunks[#chunks + 1] = string.sub(name, k, k + 13) end
		local d = invDyn(e.id)
		sent[i] = d
		msgs[#msgs + 1] = string.format("TDVRIT %d %d %d %d %s", v, i, e.g, #chunks, d)
		for k, c in ipairs(chunks) do msgs[#msgs + 1] = string.format("TDVRIN %d %d %d %s", v, i, k, c) end
	end
	inv.ids, inv.msgs, inv.pos, inv.doneT, inv.dynSent, inv.queue = ids, msgs, 1, nil, sent, {}
	inv.dynPos = nil   -- (fastLua : un passage des munitions en cours repart sur la nouvelle liste)
	inv.cur, inv.tool = nil, nil   -- l'outil en main sera renvoyé une fois la liste reçue
end

-- Outil choisi dans le casque (refusé comme le ferait le jeu : outil vide, bloqué...).
local function invSelect(id)
	local p = "game.tool." .. id
	if not GetBool(p .. ".enabled") then return end
	if HasKey(p .. ".selectable") and not GetBool(p .. ".selectable") then return end
	if GetBool("game.input.locktool") or GetPlayerGrabBody() ~= 0 or GetPlayerVehicle() ~= 0 then return end
	SetString("game.player.tool", id)
	if camDebug then debugMsg = "inventory: tool " .. id .. " taken" end
end

local function invSend(msg)
	-- (jamais plus de 30 caractères : le jeu relit les textes plus longs après les avoir libérés)
	if #msg <= 30 then pcall(SetClipboardText, msg) end
end

local function updateInventory(st, dt)
	local r = invResp
	if st.frozen or not r.ok then return end
	-- Outil choisi dans le casque : pris une seule fois.
	if inv.selSeq == nil then
		inv.selSeq = r.seq   -- (choix faits avant le chargement de ce script : ignorés)
	elseif r.seq ~= inv.selSeq then
		inv.selSeq = r.seq
		local id = r.ver == inv.ver and inv.ids[r.idx] or nil
		if id then invSelect(id) end
		inv.cur = nil   -- l'outil en main est renvoyé (le choix a pu être refusé)
	end
	local open = r.open == 1
	local opening = open and not inv.open
	if opening then inv.scanT, inv.dynT, inv.dynSent, inv.scanJob, inv.dynPos = 0, 0, {}, nil, nil end   -- ouverture : tout est relu
	inv.open = open
	inv.scanT = inv.scanT - dt
	if fastLua.on and inv.scanJob then
		-- (fastLua : relecture périodique étalée, 12 outils par image ; à l'ouverture, tout de suite)
		local list, job = invScan(inv.scanJob, 12)
		inv.scanJob = job
		if list then invBuild(list) end
	elseif inv.scanT <= 0 then
		inv.scanT = open and 0.5 or 2
		if fastLua.on and not opening then
			local list, job = invScan(nil, 12)
			inv.scanJob = job
			if list then invBuild(list) end
		else
			invBuild(invScan())
		end
	end
	local budget = INV_BUDGET
	if not inv.doneT or r.ack ~= inv.ver then
		-- La liste part (et repart si la DLL ne l'a pas toute reçue au bout d'une seconde).
		if inv.doneT and GetTime() - inv.doneT > 1 then inv.pos, inv.doneT = 1, nil end
		while budget > 0 and inv.pos <= #inv.msgs do
			invSend(inv.msgs[inv.pos])
			inv.pos, budget = inv.pos + 1, budget - 1
		end
		if inv.pos > #inv.msgs and not inv.doneT then inv.doneT = GetTime() end
		return
	end
	-- Liste reçue : munitions et disponibilité (panneau ouvert), outil en main.
	if open then
		inv.dynT = inv.dynT - dt
		if fastLua.on and inv.dynPos then
			inv.dynPos = inv.dynPass(inv.dynPos, 8)   -- (fastLua : passage en cours, 8 outils par image)
		elseif inv.dynT <= 0 then
			inv.dynT = 0.2
			if fastLua.on and next(inv.dynSent) ~= nil then
				inv.dynPos = inv.dynPass(1, 8)
			else
				inv.dynPass(1, nil)   -- (le premier passage après l'ouverture : tout de suite)
			end
		end
	end
	local tool = GetString("game.player.tool")
	if tool ~= inv.tool then
		inv.tool, inv.curIdx = tool, 0
		for i, id in ipairs(inv.ids) do
			if id == tool then
				inv.curIdx = i
				break
			end
		end
	end
	local locked = (GetBool("game.input.locktool") or GetPlayerGrabBody() ~= 0) and 1 or 0
	if inv.curIdx * 2 + locked ~= inv.cur then
		inv.cur = inv.curIdx * 2 + locked
		inv.queue[#inv.queue + 1] = string.format("TDVRIC %d %d %d", inv.ver, inv.curIdx, locked)
	end
	local q, len = inv.queue, #inv.queue
	local n = math.min(budget, len)
	for k = 1, n do invSend(q[k]) end
	for k = 1, len do q[k] = q[k + n] end   -- (le reste partira à l'image suivante)
end

local tokens = {}

-- Envoie une requête à la DLL et lit l'état VR de cette image.
local function request(flags)
	if failures > 30 and GetTime() < retryAt then return nil end
	seq = seq % 99999 + 1
	local msg = string.format("%s %d %d %.2f %.2f %.2f", PROTO, seq, flags, hapL, hapR, vignette)
	if debugMsg then
		-- Message de diagnostic : envoyé à part (il est long, il peut se perdre).
		pcall(SetClipboardText, "TDVRL " .. debugMsg)
		debugMsg = nil
	end
	local s = exchange(msg)
	hapL, hapR = 0, 0
	if s == nil then return nil end
	local t, n = tokens, 0   -- table réutilisée d'une image à l'autre
	for w in string.gmatch(s, "%S+") do
		n = n + 1
		t[n] = w
	end
	for i = n + 1, #t do t[i] = nil end
	-- (fastLua : étiquettes indexées une fois pour toutes les lectures de cette réponse)
	fastLua.tagsOk = false
	if fastLua.on then
		fastLua.index(t, n)
		fastLua.tagsOk = true
	end
	invParse(t, n)   -- champs de l'inventaire VR (voir plus haut)
	local piv, pivKey = pivParse(t, n)
	if num(t, 3) == 2 then return { paused = true } end   -- carte du niveau : affichée sur l'écran virtuel
	if num(t, 3) ~= 1 or n < 79 then return nil end
	local fast = math.floor(num(t, 80) / 64) % 2 == 1   -- réglage fastLua de la DLL
	fastLua.on = fast
	return {
		fast = fast,
		eye = num(t, 4), fovH = num(t, 5), fovV = num(t, 6), refHeadY = num(t, 7), recenter = num(t, 8),
		eyePose = pose(t, 9, fast), head = pose(t, 16, fast),
		laim = pose(t, 23, fast), raim = pose(t, 30, fast), lgrip = pose(t, 37, fast), rgrip = pose(t, 44, fast),
		lsx = num(t, 51), lsy = num(t, 52), rsx = num(t, 53), rsy = num(t, 54),
		ltrig = num(t, 55), rtrig = num(t, 56), lsq = num(t, 57), rsq = num(t, 58), buttons = num(t, 59),
		turnMode = num(t, 60), snapAngle = num(t, 61), smoothTurnSpeed = num(t, 62),
		moveDirection = num(t, 63), moveSpeed = num(t, 64), roomscale = num(t, 65),
		heightOffset = num(t, 66), physicalHeight = num(t, 67), worldScale = num(t, 68),
		leftHanded = num(t, 69), aimMode = num(t, 70), hideHud = num(t, 71), laser = num(t, 72),
		toolOff = Vec(num(t, 73), num(t, 74), num(t, 75)), toolPitch = num(t, 76),
		vignette = num(t, 77), vignetteStrength = num(t, 78), uiOpen = num(t, 79),
		debugFlags = num(t, 80), frozen = num(t, 81) == 1, removeFog = num(t, 82) == 1, swingGesture = num(t, 83) == 1,
		removeBloom = num(t, 84) == 1, toolCollision = num(t, 85) == 1, toolBlock = num(t, 86) == 1, removeDof = num(t, 87) == 1,
		toolPhysics = num(t, 88) == 1,
		-- Outil en main (DLL) : 1 outil du jeu, 2 outil d'un mod (+ sa pose en vue écran
		-- et la main droite que le mod pose dessus). 0 avec une ancienne DLL.
		toolKind = num(t, 89), toolFlat = pose(t, 90, fast), toolHandOk = num(t, 97) == 1,
		toolHand = Vec(num(t, 98), num(t, 99), num(t, 100)),
		-- Taille donnée par la DLL à l'outil d'un mod (ses formes réduites autour de la
		-- bouche de son canon, toolPivot) ; les emplacements de l'outil (fp_action...) ne le sont pas.
		toolScale = (num(t, 101) or 0) > 0 and num(t, 101) or 1,
		toolPivot = piv, toolPivotKey = pivKey,
	}
end

-- Yaw (degrés) d'une rotation : 0 = regarde vers -Z.
local function yawOf(q)
	local f = QuatRotateVec(q, Vec(0, 0, -1))
	return math.deg(math.atan2(-f[1], -f[3]))
end

-- Le balancement de la tête du jeu (options.input.headbob) secoue toute la vue
-- et l'outil à chaque pas et à chaque atterrissage : supportable sur un écran,
-- pas dans un casque. Il est coupé pendant la VR, puis remis.
local savedHeadbob = nil

local function setHeadbob(v)
	local ok, err = pcall(SetInt, "options.input.headbob", v)
	if not ok then debugMsg = "headbob: " .. tostring(err) end
end

-- Brume atmosphérique : le lointain paraît flou et délavé avec. Retirée pendant
-- la VR si le réglage est coché ; les valeurs du niveau sont remises ensuite.
local savedFog = nil
local fogApplied = false
local fogTimer = 0

local function readFog()
	local ok, a, b, c, d = pcall(GetEnvironmentProperty, "fogParams")
	local ok2, scale = pcall(GetEnvironmentProperty, "fogscale")
	local ok3, sun = pcall(GetEnvironmentProperty, "sunFogScale")
	if not ok then return nil end
	return { a, b, c, d, ok2 and scale or nil, ok3 and sun or nil }
end

local function writeFog(f, keep)
	if not f then return end
	pcall(SetEnvironmentProperty, "fogParams", f[1], f[2], (f[3] or 0) * keep, f[4])
	if f[5] then pcall(SetEnvironmentProperty, "fogscale", f[5] * keep) end
	if f[6] then pcall(SetEnvironmentProperty, "sunFogScale", f[6] * keep) end
end

-- Zoom par rapprochement : quand un mod montre sa caméra loin du joueur (avion de
-- l'AC130, drone...) et zoome, la DLL avance la caméra vers le point visé au lieu
-- d'agrandir l'image (une image agrandie dans le casque se déforme quand on tourne la
-- tête, comme un fisheye). Il lui faut la distance de ce point : un rayon le long de
-- l'axe de la caméra du mod (celle de l'image précédente), envoyé à chaque image
-- normale ("TDVRZ <mètres>", 0 = rien touché ; 30 caractères au plus, voir exchange).
-- Ignorés : les petits objets et ceux qui filent (obus, débris projetés ou qui tombent :
-- ils ne font que passer) et, tout près, l'objet qui entoure la caméra (la carlingue de
-- l'avion du mod) ; jamais le décor fixe.
local zoomRej = {}   -- objets ignorés par le rayon (table réutilisée)

local function sendModCamDistance()
	if GetBool("game.map.enabled") then return end
	local ct = GetCameraTransform(1)   -- (1 : la vraie caméra du jeu, celle du mod)
	local p = ct.pos
	local dir = QuatRotateVec(ct.rot, Vec(0, 0, -1))
	local world = GetWorldBody()
	local n, dist = 0, 0
	for attempt = 1, 4 do
		QueryRequire("visible")
		for i = 1, n do QueryRejectBody(zoomRej[i]) end
		local hit, d, _, s = QueryRaycast(p, dir, 500)
		if not hit then break end
		local b = s ~= 0 and GetShapeBody(s) or 0
		local keep = b == 0 or b == world
		if not keep then
			local mn, mx = GetBodyBounds(b)
			local small = math.max(mx[1] - mn[1], mx[2] - mn[2], mx[3] - mn[3]) < 1.5
			local around = d < 4 and p[1] > mn[1] - 1 and p[1] < mx[1] + 1 and p[2] > mn[2] - 1
				and p[2] < mx[2] + 1 and p[3] > mn[3] - 1 and p[3] < mx[3] + 1
			local fast = not small and VecLength(GetBodyVelocity(b)) > 5
			keep = not (small or around or fast)
		end
		if keep then
			dist = d
			break
		end
		n = n + 1
		zoomRej[n] = b
	end
	-- (de l'eau avant le fond : la caméra s'arrête au-dessus de sa surface)
	local ok, wh, wd = pcall(QueryRaycastWater, p, dir, dist > 0 and dist or 500)
	if ok and wh and wd > 4 then dist = wd end
	pcall(SetClipboardText, string.format("TDVRZ %.1f", dist))
end

local function updateFog(st, dt)
	-- Caméra d'un mod loin du joueur (avion de l'AC130, drone...) : le mod règle lui-même
	-- la brume de sa vue. On n'y touche pas : la vérification toutes les 2 s l'effaçait,
	-- elle apparaissait puis disparaissait. (À la sortie, le mod remet ce qu'il a trouvé.)
	if not modCamNear then
		-- (images normales seulement, comme updateFog ; pcall : une erreur ici ne doit pas
		-- arrêter le reste de tick)
		pcall(sendModCamDistance)
		return
	end
	if st.removeFog then
		if not savedFog then savedFog = readFog() end
		fogTimer = fogTimer - dt
		if not fogApplied then
			writeFog(savedFog, 0)
			fogApplied = true
			fogTimer = 2
			if camDebug then
				local f, now = savedFog or {}, readFog() or {}
				debugMsg = string.format("fog before %s %s %s %s s%s sun%s after %s %s %s %s s%s sun%s",
					tostring(f[1]), tostring(f[2]), tostring(f[3]), tostring(f[4]), tostring(f[5]), tostring(f[6]),
					tostring(now[1]), tostring(now[2]), tostring(now[3]), tostring(now[4]), tostring(now[5]), tostring(now[6]))
			end
		elseif fogTimer <= 0 then
			-- Le niveau a-t-il remis de la brume (changement de météo) ? On ne
			-- réécrit que dans ce cas (réécrire l'environnement coûte cher).
			fogTimer = 2
			local now = readFog()
			if now and now[3] and now[3] > 0.0001 then
				savedFog = now
				writeFog(savedFog, 0)
			end
		end
	elseif fogApplied then
		writeFog(savedFog, 1)
		fogApplied = false
	end
end

-- Halo lumineux (bloom) : le jeu étale la lumière du ciel en un halo qui voile
-- les objets lointains comme un flou. Retiré pendant la VR si coché, remis ensuite.
local savedBloom = nil
local bloomApplied = false
local bloomTimer = 0

local function updateBloom(st, dt)
	if st.removeBloom then
		bloomTimer = bloomTimer - dt
		if not bloomApplied or bloomTimer <= 0 then
			bloomTimer = 2
			local ok, v = pcall(GetPostProcessingProperty, "bloom")
			if ok and type(v) == "number" and v > 0.0001 then
				savedBloom = v   -- valeur du niveau (aussi après un changement par le niveau)
				pcall(SetPostProcessingProperty, "bloom", 0)
				if camDebug and not bloomApplied then debugMsg = string.format("bloom %.3f -> 0", v) end
			elseif not bloomApplied then
				pcall(SetPostProcessingProperty, "bloom", 0)
			end
			bloomApplied = true
		end
	elseif bloomApplied then
		if savedBloom then pcall(SetPostProcessingProperty, "bloom", savedBloom) end
		bloomApplied = false
	end
end

local function onActivate()
	wasActive = true
	vrYaw = GetPlayerYaw()
	lastVehYaw = nil
	local ok, v = pcall(GetInt, "options.input.headbob")
	if ok and v and v ~= 0 then
		savedHeadbob = v
		setHeadbob(0)
	end
	DebugPrint("Teardown VR: headset connected / Teardown VR : casque connecté")
end

local function onDeactivate()
	wasActive = false
	if physDestroy then physDestroy() end
	if physSetPlayerMask then physSetPlayerMask(false) end
	if offArm.destroy then offArm.destroy() end
	if offHand.destroy then
		pcall(offHand.destroy)        -- (main physique supprimée, objet tenu lâché)
		pcall(offHand.restore, true)  -- (collisions des objets lâchés remises tout de suite : le mod s'arrête)
	end
	if bloomApplied then
		if savedBloom then pcall(SetPostProcessingProperty, "bloom", savedBloom) end
		bloomApplied = false
	end
	if fogApplied then
		writeFog(savedFog, 1)
		fogApplied = false
	end
	if savedHeadbob then
		setHeadbob(savedHeadbob)
		savedHeadbob = nil
	end
	-- (armes doubles : l'outil retrouve son autre arme et la copie disparaît - sinon elle resterait en
	-- l'air, et l'outil sans son autre arme, sur l'écran du PC ; tdvrDual est global : pas d'upvalue)
	if tdvrDual and tdvrDual.stop and (tdvrDual.body ~= 0 or tdvrDual.copy ~= 0) then
		pcall(tdvrDual.stop, "VR stopped")
	end
end

-- ---------------------------------------------------------------------------
-- Rotation (joystick de la main non dominante = déplacement, dominante = rotation)
-- ---------------------------------------------------------------------------
local function updateTurning(st, dt)
	local sx = st.rsx
	if st.leftHanded == 1 then sx = st.lsx end
	if st.turnMode == 0 then
		if math.abs(sx) > 0.7 and snapReady then
			vrYaw = vrYaw - (sx > 0 and 1 or -1) * st.snapAngle
			snapReady = false
		elseif math.abs(sx) < 0.3 then
			snapReady = true
		end
	else
		if math.abs(sx) > 0.15 then
			vrYaw = vrYaw - sx * st.smoothTurnSpeed * dt
		end
	end
	if vrYaw > 180 then vrYaw = vrYaw - 360 end
	if vrYaw < -180 then vrYaw = vrYaw + 360 end
end

-- ---------------------------------------------------------------------------
-- Déplacement au joystick
-- ---------------------------------------------------------------------------
-- Marche automatique : en VR le personnage ne marche qu'avec le joystick (vitesse donnée
-- ci-dessous). Une entrée de marche du jeu restée « enfoncée » (relâchement de touche perdu,
-- manette virtuelle de Virtual Desktop...) faisait marcher le personnage à fond tout seul
-- (6,5 à 7 m/s mesurés, plus que les 4,5 m/s du joystick), sans que le joystick puisse
-- l'arrêter. Tant que le jeu voit une touche de déplacement enfoncée, sa vitesse de marche à
-- lui est mise à zéro (seulement à ce moment-là : à zéro en permanence, elle bloquait aussi
-- la marche au joystick), et la DLL relâche les touches de déplacement (TDVRU) ; le journal
-- dit d'où elle vient.
local autoWalk = { t = 0, logged = 0 }
local function neutralizeGameWalk(dt)
	local up, down, left, right = InputDown("up"), InputDown("down"), InputDown("left"), InputDown("right")
	if not (up or down or left or right) then
		autoWalk.t = 0
		return
	end
	pcall(SetPlayerWalkingSpeed, 0)
	autoWalk.t = autoWalk.t + dt
	if autoWalk.t < 0.5 then return end
	autoWalk.t = -4   -- (au plus une fois toutes les 4 s)
	pcall(SetClipboardText, "TDVRU")
	if autoWalk.logged < 20 and not debugMsg then
		autoWalk.logged = autoWalk.logged + 1
		local okDev, dev = pcall(GetString, "game.input.curdevice")
		debugMsg = string.format("auto-walk: the game sees movement keys held (up %s down %s left %s right %s, device %s) - released, game walking speed kept at 0",
			tostring(up), tostring(down), tostring(left), tostring(right), okDev and tostring(dev) or "?")
	end
end

local function updateLocomotion(st, toWorld, dt)
	neutralizeGameWalk(dt)
	local mx, my = st.lsx, st.lsy
	if st.leftHanded == 1 then mx, my = st.rsx, st.rsy end
	local mag = math.sqrt(mx * mx + my * my)
	if mag < 0.15 then
		vignette = math.max(0, vignette - dt * 4)
		return
	end
	if mag > 1 then mx, my, mag = mx / mag, my / mag, 1 end

	local ref
	if st.moveDirection == 1 then
		ref = toWorld(st.leftHanded == 1 and st.rgrip or st.lgrip).rot
	else
		ref = toWorld(st.head).rot
	end
	local fwd = QuatRotateVec(ref, Vec(0, 0, -1))
	fwd = VecNormalize(Vec(fwd[1], 0, fwd[3]))
	local right = Vec(-fwd[3], 0, fwd[1])
	local dir = VecAdd(VecScale(fwd, my), VecScale(right, mx))

	local speed = 4.5 * st.moveSpeed   -- m/s : confortable en VR (le jeu marche à ~6,5 m/s)
	if GetPlayerCrouch() > 0.5 then speed = speed * 0.5 end
	local v = GetPlayerVelocity()
	local want = VecScale(dir, speed)
	SetPlayerVelocity(Vec(want[1], v[2], want[3]))
	if camDebug then
		moveProbe = (moveProbe or 0) + 1
		if moveProbe % 60 == 0 then
			local p = GetPlayerTransform().pos
			local t = GetTime()
			if lastProbePos then
				local d = VecLength(Vec(p[1] - lastProbePos[1], 0, p[3] - lastProbePos[3]))
				debugMsg = string.format("move want=%.2f vel=%.2f measured=%.2f m/s", VecLength(want),
					VecLength(Vec(v[1], 0, v[3])), d / math.max(t - lastProbeT, 0.001))
			end
			lastProbePos, lastProbeT = p, t
		end
	end
	vignette = math.min(1, vignette + dt * 4) * mag
end

-- ---------------------------------------------------------------------------
-- Visée : le joueur "regarde" là où pointe la manette
-- ---------------------------------------------------------------------------
local lastAimYaw, lastAimPitch = nil, nil
local aimSetThisTick = false
local noAimDebug = false
local moveProbe, lastProbePos, lastProbeT = 0, nil, 0
local yawWriter = false   -- la DLL sait tourner le joueur sans le déplacer

-- Oriente le joueur (et le déplace de "move" pour le roomscale).
local function setPlayerAim(yaw, pitch, move)
	if not move and lastAimYaw and math.abs(yaw - lastAimYaw) < 0.05 and math.abs(pitch - lastAimPitch) < 0.05 then
		-- Rien à changer... sauf si le jeu a tourné le joueur entre-temps (la souris
		-- simulée par un joystick pour la caméra d'un mod, un autre mod...).
		local drift = (GetPlayerYaw() - lastAimYaw) % 360
		if (drift < 1 or drift > 359) and math.abs(GetPlayerPitch() - lastAimPitch) < 1 then return end
	end
	lastAimYaw, lastAimPitch = yaw, pitch
	aimSetThisTick = true
	if move or not yawWriter then
		-- Déplacement roomscale : on repositionne le joueur (rare, voir roomScaleStep).
		local pt = GetPlayerTransform()
		local vel = GetPlayerVelocity()
		local pos = move and VecAdd(pt.pos, move) or pt.pos
		SetPlayerTransformWithPitch(Transform(pos, QuatEuler(pitch, yaw, 0)))
		SetPlayerVelocity(vel)
	else
		-- Seulement l'orientation : repositionner le joueur à chaque image (la main
		-- bouge toujours un peu) faisait trembler la caméra et l'outil (mesuré :
		-- à-coups jusqu'à 39 mm, immobile).
		pcall(SetClipboardText, string.format("TDVRY %.3f", yaw)) -- la DLL écrit le lacet du joueur
		SetPlayerPitch(pitch)
	end
end

-- Les autres scripts (armes des mods, frappe aérienne de l'AC130 au sol, mode
-- créatif...) visent avec GetCameraTransform(), qui est en VR la caméra de la tête.
-- La DLL leur donne à la place la caméra du joueur (à pied, sans caméra de mod ni
-- carte), que setPlayerAim tourne vers la cible du laser : ils visent avec la
-- manette, comme les armes du jeu (voir engine.cpp). Ce script lit la vraie caméra
-- (l'œil) avec GetCameraTransform(1) : avec un argument (ignoré par le jeu), la DLL
-- ne remplace rien.
-- Diagnostic (debugLua) : la caméra donnée aux autres scripts doit être celle du
-- joueur, et son axe doit passer par la cible du laser (celle de cette image ou de
-- la précédente : la visée donnée au joueur peut n'être appliquée qu'à l'image
-- suivante). Main immobile : écart de l'ordre du centimètre (cible à 20 m).
local aimCam = { n = 0, same = 0, maxD = 0, sumD = 0, prev = nil }

-- Distance du point p à l'axe (origine o, direction f unitaire).
local function axisDist(p, o, f)
	local v = VecSub(p, o)
	return VecLength(VecSub(v, VecScale(f, VecDot(v, f))))
end

local function checkAimCamera(target)
	local ac = GetCameraTransform()   -- sans argument : ce que lisent les autres mods
	local pc = GetPlayerCameraTransform()
	local f = QuatRotateVec(ac.rot, Vec(0, 0, -1))
	if VecLength(VecSub(ac.pos, pc.pos)) < 0.001 and VecDot(f, QuatRotateVec(pc.rot, Vec(0, 0, -1))) > 0.99999 then
		aimCam.same = aimCam.same + 1
	end
	local d = axisDist(target, ac.pos, f)
	if aimCam.prev then d = math.min(d, axisDist(aimCam.prev, ac.pos, f)) end
	aimCam.maxD = math.max(aimCam.maxD, d)
	aimCam.sumD = aimCam.sumD + d
	aimCam.prev = target
	aimCam.n = aimCam.n + 1
	if aimCam.n >= 240 and not debugMsg then
		debugMsg = string.format("aimcam %d/%d=player cam, laser off axis max %.3f mean %.3f m",
			aimCam.same, aimCam.n, aimCam.maxD, aimCam.sumD / aimCam.n)
		aimCam.n, aimCam.same, aimCam.maxD, aimCam.sumD = 0, 0, 0, 0
	end
end

local function updateAim(st, toWorld, cam, frozen, move)
	local origin, dir
	if st.aimMode == 1 then
		origin = cam.pos
		dir = QuatRotateVec(cam.rot, Vec(0, 0, -1))
	else
		local aim = toWorld(st.leftHanded == 1 and st.laim or st.raim)
		origin = aim.pos
		dir = QuatRotateVec(aim.rot, Vec(0, 0, -1))
	end
	QueryRequire("physical visible")
	local tb = GetToolBody()
	if tb ~= 0 then QueryRejectBody(tb) end
	rejectPhys()
	local hit, dist = QueryRaycast(origin, dir, 150)
	if not hit then dist = 150 end
	local target = VecAdd(origin, VecScale(dir, dist))
	if not frozen and tdvrDual then tdvrDual.rTarget = target end   -- (armes doubles : cible de l'arme de cette main)

	local pc = GetPlayerCameraTransform()
	local d = VecNormalize(VecSub(target, pc.pos))
	local yaw = math.deg(math.atan2(-d[1], -d[3]))
	local pitch = math.deg(math.asin(clamp(d[2], -1, 1)))
	if not frozen and not noAimDebug then setPlayerAim(yaw, clamp(pitch, -89, 89), move) end
	if camDebug and not frozen and st.aimMode == 0 then checkAimCamera(target) end
	-- Viseurs 2D des armes de mods (logo du portal gun, viseur du chaingun...) : ils
	-- sont dessinés au centre de l'écran, droit devant la TÊTE en VR, alors que l'arme
	-- tire vers la cible du laser. La DLL les place sur cette cible (envoyée ici,
	-- relative à l'œil de cette image ; l'œil droit reprend celle de l'œil gauche).
	-- Message de 30 caractères au plus (voir exchange).
	if not frozen and st.aimMode == 0 then
		local r = VecSub(target, cam.pos)
		local m = math.max(math.abs(r[1]), math.abs(r[2]), math.abs(r[3]))
		local msg = string.format(m < 99 and "TDVRT %.3f %.3f %.3f" or "TDVRT %.2f %.2f %.2f", r[1], r[2], r[3])
		if #msg <= 30 then pcall(SetClipboardText, msg) end
	end

	if st.laser == 1 and st.aimMode == 0 then
		local stop = VecAdd(origin, VecScale(dir, math.min(dist, 8)))
		DrawLine(VecAdd(origin, VecScale(dir, 0.08)), stop, 1, 0.55, 0.1, 0.35)
		if hit then
			local s = 0.012 * math.max(1, dist * 0.6)
			DrawLine(VecAdd(target, Vec(-s, 0, 0)), VecAdd(target, Vec(s, 0, 0)), 1, 0.6, 0.15, 1)
			DrawLine(VecAdd(target, Vec(0, -s, 0)), VecAdd(target, Vec(0, s, 0)), 1, 0.6, 0.15, 1)
			DrawLine(VecAdd(target, Vec(0, 0, -s)), VecAdd(target, Vec(0, 0, s)), 1, 0.6, 0.15, 1)
		end
	end
end

-- ---------------------------------------------------------------------------
-- Roomscale : quand on marche dans la pièce, le corps du joueur suit la tête
-- ---------------------------------------------------------------------------
local roomOffset = Vec(0, 0, 0) -- déplacements réels déjà donnés au corps (repère de la pièce)

local function roomMoveAllowed(pos, d)
	local len = VecLength(d)
	if len < 0.0001 then return false end
	local dir = VecScale(d, 1 / len)
	QueryRequire("physical large")
	local tb = GetToolBody()
	if tb ~= 0 then QueryRejectBody(tb) end
	local gb = GetPlayerGrabBody()
	if gb ~= 0 then QueryRejectBody(gb) end
	rejectPhys()
	local hit, _, n = QueryRaycast(VecAdd(pos, Vec(0, 0.9, 0)), dir, len + 0.3, 0.2)
	return not hit or VecDot(n, dir) > -0.2
end

-- Renvoie le déplacement à donner au corps (monde), ou nil.
local function roomScaleStep(st, anchorRot, scale, pos)
	local h = Vec(st.head.pos[1] - roomOffset[1], 0, st.head.pos[3] - roomOffset[3])
	-- Le corps ne rejoint la tête que si elle s'en éloigne de plus de 10 cm (on
	-- ne repositionne pas le joueur pour chaque petit mouvement de tête).
	if VecLength(h) < 0.10 then return nil end
	local d = QuatRotateVec(anchorRot, VecScale(h, scale))
	if not roomMoveAllowed(pos, d) then return nil end
	roomOffset = VecAdd(roomOffset, h)
	return d
end

-- ---------------------------------------------------------------------------
-- Outil dans la main dominante
-- ---------------------------------------------------------------------------
-- frame : repère du joueur tel qu'il sera rendu (pieds + 1,7 m, orientation horizontale).
local collideTool
local lastShownT = nil
local lastShownLocal = nil   -- pose de l'outil relative au corps du joueur (image de l'œil gauche)

-- ---------------------------------------------------------------------------
-- Outils ajoutés par des mods (armes, gadgets...). Le jeu ne sait pas comment on
-- les tient : en vue écran, chaque mod pose lui-même son outil devant l'œil
-- (SetToolTransform), là où une main droite le tiendrait. La DLL reprend cette pose
-- « écran », animations du mod comprises (recul, rechargement...), et la rapporte
-- à la main VR : outil = main x H⁻¹ x pose écran. H est la main « virtuelle » de la
-- vue écran pour cet outil, cherchée une fois par outil dans l'outil lui-même :
--   1. ses emplacements fp_action / tp_action_rh (outils « version 2 », comme ceux du jeu) ;
--   2. sinon la main droite que l'outil ou le mod pose dessus (tp_action_rh,
--      SetToolHandPoseLocalTransform), si elle est bien sur l'outil ;
--   3. sinon le point de l'outil qui est là où les outils du jeu sont tenus en vue
--      écran, ramené sur l'outil s'il tombe à côté.
-- Rien n'est propre à un mod : tout mod installé plus tard est tenu de la même façon.
-- ---------------------------------------------------------------------------
local MOD_HAND = Vec(0.30, -0.40, -0.45)   -- main des outils du jeu en vue écran (repère de l'œil, médiane)
local MOD_BIG = 1.3                        -- m : plus grand que ça, pas de copie physique (canons géants)
local modHold = { key = nil, t = 0, H = nil, size = 0, ver = 0, since = 0, how = "", cache = {}, kinds = {}, prevName = nil, same = 0 }

-- Boîte englobante des formes visibles de l'outil, dans le repère de son corps.
local function toolBox(body)
	local lo, hi
	for _, sh in ipairs(GetBodyShapes(body) or {}) do
		local sx, sy, sz, scale = GetShapeSize(sh)
		local lt = GetShapeLocalTransform(sh, 1)   -- (1 : la forme telle qu'elle est dessinée)
		-- (une forme envoyée très loin par son mod est une forme cachée : ignorée ; un outil
		-- réduit autour de la bouche de son canon peut être à 2 ou 3 m de l'origine de son corps)
		if sx and sx > 0 and scale and scale > 0 and GetShapeVoxelCount(sh) > 0 and VecLength(lt.pos) < 10 then
			for i = 0, 7 do
				local c = Vec((i % 2) * sx * scale, (math.floor(i / 2) % 2) * sy * scale, math.floor(i / 4) * sz * scale)
				local p = TransformToParentPoint(lt, c)
				if not lo then
					lo, hi = VecCopy(p), VecCopy(p)
				else
					for a = 1, 3 do
						lo[a] = math.min(lo[a], p[a])
						hi[a] = math.max(hi[a], p[a])
					end
				end
			end
		end
	end
	return lo, hi
end

-- H (repère de l'œil) de l'outil de mod en main : la main tient le point H de l'outil
-- tel qu'il est en vue écran (position seule, sauf avec fp_action).
local function modToolHold(st, dt)
	local body = GetToolBody()
	local name = GetString("game.player.tool")
	-- Une prise par sortie de l'outil (nouveau corps). Le nombre de formes n'y entre pas :
	-- un mod qui ajoute ou retire des formes en tirant (éclair, douilles...) ne doit pas
	-- faire sauter l'outil dans la main.
	-- (taille arrondie au centième, mise en texte seulement quand elle change ; la clé est
	-- comparée champ par champ : rien n'est construit à chaque image)
	local s = st.toolScale or 1
	if s ~= modHold.scaleRaw then modHold.scaleRaw, modHold.scaleTxt = s, string.format("%.2f", s) end
	local sTxt = modHold.scaleTxt
	if not modHold.key or name ~= modHold.keyName or body ~= modHold.keyBody or sTxt ~= modHold.keyScale then
		-- (outil déjà tenu à cette taille : même prise tout de suite)
		local c = modHold.cache[name .. ":" .. sTxt]
		modHold.keyName, modHold.keyBody, modHold.keyScale = name, body, sTxt
		modHold.key, modHold.t, modHold.since = name .. ":" .. body .. ":" .. sTxt, 0, 0
		modHold.H, modHold.size, modHold.how = c and c.H, c and c.size or 0, c and c.how or ""
		modHold.ver = modHold.ver + 1
	end
	if modHold.H then return modHold.H end
	-- On laisse au jeu le temps de créer l'outil et au mod celui de le poser.
	modHold.t = modHold.t + dt
	if modHold.t < 0.3 or body == 0 or st.toolKind ~= 2 then return Transform(MOD_HAND) end
	local flat = st.toolFlat
	local lo, hi = toolBox(body)
	if lo then modHold.size = math.max(hi[1] - lo[1], hi[2] - lo[2], hi[3] - lo[3]) end
	-- Point ramené dans la boîte de l'outil (rétrécie de « marge » de chaque côté).
	local function onTool(p, margin)
		if not lo then return p end
		local r = Vec(0, 0, 0)
		for a = 1, 3 do
			local m = (hi[a] - lo[a]) * margin
			r[a] = clamp(p[a], lo[a] + m, hi[a] - m)
		end
		return r
	end
	local fp = GetToolLocationLocalTransform("fp_action")
	local rh = GetToolLocationLocalTransform("tp_action_rh")
	local ts = st.toolScale or 1
	if fp then fp = Transform(VecScale(fp.pos, ts), fp.rot) end
	if rh then rh = Transform(VecScale(rh.pos, ts), rh.rot) end
	local H
	if fp and rh then
		H = TransformToParentTransform(fp, Transform(rh.pos))
		modHold.how = "prefab"
	else
		local g = onTool(TransformToLocalPoint(flat, MOD_HAND), 0.2)
		modHold.how = "auto"
		-- Main donnée par l'outil ou par le mod : gardée si elle est bien sur l'outil (à
		-- 10 cm près ; certains mods la placent pour le bras du personnage, à côté).
		-- (l'emplacement tp_action_rh, dans le corps de l'outil, réduit autour de sa bouche)
		local hand = rh and VecAdd(rh.pos, VecScale(st.toolPivot or Vec(0, 0, 0), 1 - ts))
			or (st.toolHandOk and st.toolHand or nil)
		if hand then
			local on = onTool(hand, 0)
			if VecLength(VecSub(on, hand)) < 0.1 then g, modHold.how = on, rh and "prefab hand" or "mod hand" end
		end
		H = Transform(TransformToParentPoint(flat, g))
	end
	modHold.H = H
	modHold.ver = modHold.ver + 1
	modHold.since = 0
	modHold.cache[name .. string.format(":%.2f", st.toolScale or 1)] = { H = H, size = modHold.size, how = modHold.how }
	if camDebug then
		debugMsg = string.format("mod tool %s: grip %s, desktop pose %.2f %.2f %.2f, hand point %.2f %.2f %.2f, size %.2f m",
			GetString("game.player.tool"), modHold.how, flat.pos[1], flat.pos[2], flat.pos[3], H.pos[1], H.pos[2], H.pos[3],
			modHold.size)
	end
	return H
end

local function updateTool(st, toWorld, frame, frozen, dt)
	if st.aimMode == 1 then return end
	-- Outil d'un mod ? La DLL le dit pour l'image précédente : c'est retenu par nom
	-- d'outil (une fois l'outil inchangé depuis deux images, le jeu changeant le corps
	-- de l'outil après son nom), pour qu'un changement d'outil ne décale rien.
	local toolName = GetString("game.player.tool")
	if toolName == modHold.prevName then modHold.same = modHold.same + 1 else modHold.same = 0 end
	modHold.prevName = toolName
	-- Coup de masse du jeu à la gâchette (rétro-ingénierie de Sledge::update, voir la DLL,
	-- input.cpp SledgeUse) : le jeu lance le coup à l'appui du bouton, mais ne cherche ce qu'il
	-- frappe que 0,26 s plus tard (sphère de 0,3 m lancée de la caméra du joueur, jusqu'à 3 m),
	-- bouton tenu ou non. Une gâchette lâchée avant rendait ses collisions à la copie physique
	-- de l'outil (physTool), qui suit le coup devant la caméra : le coup touchait la copie
	-- (incassable) au lieu de l'objet visé (« il fait le geste mais ne casse rien »). La copie,
	-- et la main libre sur ce chemin (tdvrOffHandTick), restent donc fantômes 0,4 s (temps du
	-- jeu) après chaque appui.
	if not frozen then
		local useDown = InputDown("usetool")
		phys.strikeT = math.max(0, (phys.strikeT or 0) - dt)
		if toolName == "sledge" and (InputPressed("usetool") or (useDown and not phys.useDown)) then
			phys.strikeT = 0.4
			if camDebug then
				-- Diagnostic : ce que ce coup doit frapper (la sphère du jeu, sans nos corps) et ses
				-- voxels, comparés à la fin de la fenêtre (ligne « sledge strike » du journal).
				local pc = GetPlayerCameraTransform()
				local tb = GetToolBody()
				if tb ~= 0 then QueryRejectBody(tb) end
				for _, pb in ipairs(GetPlayerBodies() or {}) do QueryRejectBody(pb) end
				rejectPhys()
				local hit, d, _, sh = QueryRaycast(pc.pos, QuatRotateVec(pc.rot, Vec(0, 0, -1)), 3, 0.3)
				phys.strike = { shape = hit and sh or 0, dist = hit and d or -1, vox = hit and GetShapeVoxelCount(sh) or 0 }
			end
		elseif phys.strike and phys.strikeT == 0 then
			local s = phys.strike
			local after = s.shape ~= 0 and IsHandleValid(s.shape) and GetShapeVoxelCount(s.shape) or 0
			pcall(SetClipboardText, string.format("TDVRL sledge strike: target shape %d at %.2f m, voxels %d -> %d (%d broken), button %s at the end",
				s.shape, s.dist, s.vox, after, s.vox - after, useDown and "still down" or "released"))
			phys.strike = nil
		end
		phys.useDown = useDown
	end
	if modHold.same >= 2 and st.toolKind ~= 0 then modHold.kinds[toolName] = st.toolKind end
	local modTool = nil
	if (modHold.kinds[toolName] or st.toolKind) == 2 then
		modTool = modToolHold(st, dt)
		if not frozen then modHold.since = modHold.since + 1 end
	end
	-- Copie physique et collisions : pas pour un outil de mod géant, ni avant de savoir où le
	-- tenir, ni avant que l'outil ait été dessiné une fois avec cette prise (elles mesurent
	-- l'outil dessiné à l'image précédente : sinon l'écart serait celui de l'ancienne prise).
	local physOk = not modTool or (modHold.H ~= nil and modHold.since >= 2 and modHold.size <= MOD_BIG)
	-- L'outil suit la main dominante : on transmet la pose de la main VR, la DLL
	-- y accroche l'outil comme le jeu le tient en vue normale (animations comprises).
	-- Les armes suivent le rayon de visée (là où partent les tirs) : l'axe de la
	-- poignée de la manette est incliné d'environ 60° vers le haut par rapport à lui.
	-- La masse reste dans l'axe du poing, comme un vrai manche.
	local lh = st.leftHanded == 1
	local hand = toWorld(lh and st.lgrip or st.rgrip)
	if GetString("game.player.tool") ~= "sledge" then
		hand = Transform(hand.pos, toWorld(lh and st.laim or st.raim).rot)
	end
	local target = TransformToParentTransform(hand, Transform(st.toolOff, QuatEuler(st.toolPitch, 0, 0)))
	-- L'outil ne traverse rien (calculé sur les images normales ; l'image de l'œil
	-- droit montre le même instant, elle reprend le résultat).
	local shown = target
	if st.toolCollision and st.toolPhysics and physTool and physOk then
		-- Image de l'œil droit : même instant, même pose relative au corps (voir plus bas).
		if frozen and lastShownLocal then
			SetToolTransformOverride(lastShownLocal)
			return target
		end
		if not frozen then shown = physTool(st, dt, target) end
	elseif st.toolCollision and collideTool and physOk then
		if phys.body ~= 0 then physDestroy() end
		-- Image de l'œil droit : même instant que l'œil gauche, mais le corps du joueur a
		-- pu avancer d'un pas de physique entre les deux. On réutilise donc la pose de
		-- l'outil RELATIVE au corps (sinon il sautait en arrière dans cet œil en marchant).
		if frozen and lastShownLocal then
			SetToolTransformOverride(lastShownLocal)
			return target
		end
		if not frozen then shown = collideTool(st, dt, target) end
	elseif phys.body ~= 0 then
		physDestroy()
	end
	-- Diagnostic du tremblement en marchant : où le jeu a dessiné l'outil (image
	-- précédente) par rapport à là où on l'avait demandé.
	if camDebug and not frozen and lastShownT then
		local body = GetToolBody()
		if body ~= 0 then
			-- écart (dans le repère de la main) entre l'outil dessiné et la main demandée ;
			-- sa variation d'une image à l'autre est le tremblement.
			-- (dans le repère de la caméra telle que dessinée : ce que voit le joueur)
			local ev = TransformToLocalPoint(GetCameraTransform(1), GetBodyTransform(body).pos)   -- (1 : vraie caméra)
			if toolDbgPrev then
				local j = VecLength(VecSub(ev, toolDbgPrev))
				toolDbgMax = math.max(toolDbgMax or 0, j)
				toolDbgSum = (toolDbgSum or 0) + j
				toolDbgN = (toolDbgN or 0) + 1
			end
			toolDbgPrev = ev
			toolDbgT = (toolDbgT or 0) + dt
			if toolDbgT > 1 and not debugMsg and toolDbgN and toolDbgN > 0 then
				debugMsg = string.format("tool jitter: frame-to-frame max %.4f m mean %.4f m, offset %.3f %.3f %.3f, speed %.2f",
					toolDbgMax, toolDbgSum / toolDbgN, ev[1], ev[2], ev[3], VecLength(GetPlayerVelocity()))
				toolDbgT, toolDbgMax, toolDbgSum, toolDbgN = 0, 0, 0, 0
			end
		end
	end
	-- Outil d'un mod : la DLL le dessine à « main donnée x pose écran » ; on lui donne
	-- donc la main déjà décalée de H⁻¹ (le point H de l'outil arrive dans la main).
	local give = shown
	if modTool then give = TransformToParentTransform(shown, TransformToLocalTransform(modTool, Transform())) end
	local shownLocal = TransformToLocalTransform(frame, give)
	if not frozen then
		lastShownT = shown
		lastShownLocal = shownLocal
	end
	SetToolTransformOverride(shownLocal)
	return target   -- la main (pour la masse physique)
end

-- ---------------------------------------------------------------------------
-- Masse physique : la tête de la masse casse ce qu'elle touche, dans tous les
-- sens, avec les effets d'un coup de masse du jeu (rétro-ingénierie de
-- Sledge::update) : trou MakeHole(P, r, 0, 0) qui ne casse que les matériaux
-- tendres (pas le métal ni le béton), poussée de 200 sur les objets à moins
-- d'un mètre (5 m/s au plus), son et éclats selon le matériau.
-- La tête est une boîte de 15 x 35 x 15 cm, couverte par 3 sphères (repère de
-- l'outil) ; la main tient le manche en (-0,025 ; -0,025 ; 0,05), à 72,5 cm de la tête ; la DLL
-- place la masse ainsi (pas d'autre rotation quand elle est au repos).
-- ---------------------------------------------------------------------------
local SLEDGE_GRIP = Transform(Vec(0.025, 0.025, -0.05))   -- inverse du point tenu (sur le manche)
local HEAD = { Vec(-0.025, -0.125, -0.675), Vec(-0.025, -0.025, -0.675), Vec(-0.025, 0.075, -0.675) }
local HEAD_R = 0.075
local MATF = { glass = "glass", wood = "wood", masonry = "masonry", plaster = "masonry", rock = "masonry",
	hardmasonry = "masonry", none = "masonry", metal = "metal", heavymetal = "metal", hardmetal = "metal",
	dirt = "dirt", foliage = "foliage", plastic = "plastic", unphysical = "plastic", ice = "ice" }
local HAPT = { glass = 0.2, wood = 0.5, masonry = 0.5, plaster = 0.5, metal = 0.7, dirt = 0.5 }
local hitSnd, swingSnd = nil, nil
local sledgePrev, sledgeCool, whooshCool, sledgeHapT, sledgeHapA = nil, 0, 0, 0, 0
local sledgeCheckT = 0
local toolPushedBody = 0   -- corps déjà poussé par un coup de masse pendant cette image

local function loadSledgeSounds()
	if hitSnd then return end
	hitSnd = {}
	for _, f in ipairs({ "wood", "masonry", "metal", "glass", "foliage", "plastic", "dirt", "ice" }) do
		hitSnd[f] = { LoadSound(f .. "/hit-s0.ogg"), LoadSound(f .. "/hit-m0.ogg"), LoadSound(f .. "/hit-l0.ogg") }
	end
	swingSnd = LoadSound("tools/sledge0.ogg")
end

local function rndUnit()
	local z = math.random() * 2 - 1
	local a = math.random() * 6.2831853
	local r = math.sqrt(1 - z * z)
	return Vec(r * math.cos(a), r * math.sin(a), z)
end

local function sledgeImpact(best, k)
	local P, n = best.p, best.n
	local mat, r, g, b, a = GetShapeMaterialAtPosition(best.shape, VecSub(P, VecScale(n, 0.03)))
	local vc = GetShapeVoxelCount(best.shape)
	local bank = hitSnd[MATF[mat] or "masonry"]
	PlaySound(bank[vc < 100 and 1 or (vc < 400 and 2 or 3)], P, 0.4 + 0.6 * k)
	local removed = 0
	if best.vImp >= 3.0 then removed = MakeHole(P, 0.25 + 0.75 * k, 0, 0) or 0 end
	-- Poussée de l'objet frappé seulement (le jeu pousse tout ce qui est à moins d'un
	-- mètre, ce qui secouait aussi l'objet sous les pieds) : 200 N.s au plus, point
	-- limité à 5 m/s, plus faible si le coup est lent.
	local dir = VecNormalize(best.v)
	local J = 200 * (0.3 + 0.7 * k)
	local bd = GetShapeBody(best.shape)
	local pushed = 0
	if bd ~= 0 and IsBodyDynamic(bd) then
		local room = math.max(0, 5.0 - VecDot(GetBodyVelocityAtPos(bd, P), dir))
		local imp = math.min(J, GetBodyMass(bd) * room)
		if imp > 0 then
			ApplyBodyImpulse(bd, P, VecScale(dir, imp))
			pushed = 1
		end
	end
	toolPushedBody = bd
	if removed > 0 and r then
		ParticleReset()
		ParticleTile(6); ParticleColor(r, g, b); ParticleAlpha(a or 1); ParticleGravity(-10)
		ParticleCollide(1); ParticleSticky(0.1)
		for i = 1, 40 do
			local u = math.random()
			ParticleRadius(0.01 + 0.05 * u * u, 0.001)
			SpawnParticle(VecAdd(VecAdd(P, VecScale(rndUnit(), 0.4 * math.random())), VecScale(n, 0.05)),
				VecScale(rndUnit(), 2.5 * math.random() ^ 2), 1 + math.random())
		end
		ParticleReset()
		ParticleType("smoke"); ParticleColor(0.5 * r + 0.4, 0.5 * g + 0.425, 0.5 * b + 0.45)
		ParticleAlpha(0.4, 0); ParticleRadius(0.1, 0.3)
		for i = 1, 16 do
			ParticleGravity(-9 * math.random())
			SpawnParticle(VecAdd(P, VecScale(n, 0.05)), VecScale(rndUnit(), 2.5 * math.random() ^ 2), 0.8 + 0.6 * math.random())
		end
	end
	sledgeHapT = 0.15
	sledgeHapA = math.max(0.3, (HAPT[mat] or 0.4) * (0.5 + 0.5 * k))
	if camDebug then
		debugMsg = string.format("sledge hit %s speed %.1f m/s k %.2f voxels %d pushed %d", tostring(mat), best.vImp, k,
			removed, pushed)
	end
end

-- target : transformation (monde) de la main passée au jeu pour l'outil.
local function updateSledge(st, dt, target)
	sledgeCool = math.max(0, sledgeCool - dt)
	whooshCool = math.max(0, whooshCool - dt)
	-- (image de durée quasi nulle : pas de vitesse mesurable, on attend la suivante)
	if dt < 0.003 and sledgePrev then return end
	if not st.swingGesture or not target or dt <= 0 or GetString("game.player.tool") ~= "sledge" then
		sledgePrev = nil
		return
	end
	loadSledgeSounds()
	local tool = TransformToParentTransform(target, SLEDGE_GRIP)
	if camDebug and not debugMsg then
		-- Vérification : la masse affichée doit être là où on la calcule (repère de la masse).
		sledgeCheckT = sledgeCheckT + dt
		local body = GetToolBody()
		if body ~= 0 and sledgeCheckT > 2 then
			sledgeCheckT = 0
			local bt = GetBodyTransform(body)
			local lp = TransformToLocalPoint(tool, bt.pos)
			local lf = TransformToLocalVec(tool, QuatRotateVec(bt.rot, Vec(0, 0, -1)))
			debugMsg = string.format("sledge check: body in tool frame %.3f %.3f %.3f fwd %.2f %.2f %.2f",
				lp[1], lp[2], lp[3], lf[1], lf[2], lf[3])
		end
	end
	local pts = {}
	for i, p in ipairs(HEAD) do pts[i] = TransformToParentPoint(tool, p) end
	if not sledgePrev or VecLength(VecSub(pts[2], sledgePrev[2])) > 1.5 then
		sledgePrev = pts   -- recentrage, rotation par crans, téléportation : on repart de zéro
		return
	end
	-- Vitesse de la tête par rapport au joueur (marcher contre un mur ne casse rien).
	local pv = GetPlayerVelocity()
	local v = VecSub(VecScale(VecSub(pts[2], sledgePrev[2]), 1 / dt), pv)
	local speed = VecLength(v)
	if speed > 4 and whooshCool == 0 then
		PlaySound(swingSnd, pts[2], math.min(1, speed / 8))
		whooshCool = 0.5
	end
	local best = nil
	if sledgeCool == 0 then
		for i = 1, #HEAD do
			local d = VecSub(pts[i], sledgePrev[i])
			local len = VecLength(d)
			if len > 0.002 then
				local dir = VecScale(d, 1 / len)
				local grab = GetPlayerGrabBody()
				if grab ~= 0 then QueryRejectBody(grab) end
				local veh = GetPlayerVehicle()
				if veh ~= 0 then QueryRejectVehicle(veh) end
				rejectPhys()
				local hit, dist, n, shape = QueryRaycast(sledgePrev[i], dir, len, HEAD_R)
				if camDebug and hit and shape ~= 0 and IsBodyDynamic(GetShapeBody(shape)) then
					debugMsg = string.format("sledge cast %d hit dynamic dist %.3f len %.3f vImp %.2f",
						i, dist, len, -VecDot(v, n))
				elseif camDebug and i == 2 and len > 0.2 then
					debugMsg = string.format("sledge sweep %.2f m from %.2f,%.2f,%.2f to %.2f,%.2f,%.2f hit %s at %.2f (feet y %.2f)",
						len, sledgePrev[i][1], sledgePrev[i][2], sledgePrev[i][3], pts[i][1], pts[i][2], pts[i][3],
						tostring(hit), dist or -1, GetPlayerTransform().pos[2])
				end
				-- dist ~ 0 : la tête était déjà dans l'objet (la main traverse les murs) ;
				-- ce n'est pas un nouveau coup (sinon un coup par 0,25 s : tremblement).
				if hit and dist > 0.005 and (not best or dist / len < best.t) then
					best = { t = dist / len, n = n, shape = shape, v = v,
						p = VecSub(VecAdd(sledgePrev[i], VecScale(dir, dist)), VecScale(n, HEAD_R)) }
				end
			end
		end
	end
	sledgePrev = pts
	if best then
		best.vImp = math.min(15, -VecDot(best.v, best.n))   -- vitesse d'approche contre la surface (bornée)
		if best.vImp >= 2.0 then
			local k = math.max(0, math.min(1, (best.vImp - 2.5) / 4.0))   -- 2,5 m/s -> 0 ; 6,5 m/s -> 1 (coup du jeu)
			sledgeImpact(best, k)
			sledgeCool = 0.25
		end
	end
end

-- ---------------------------------------------------------------------------
-- Outil physiquement présent : l'outil en main (masse, armes, ...) pousse les
-- objets mobiles qu'il touche, dans tous les sens, avec la force d'un bras
-- (impulsion limitée : un carton part, une voiture bouge à peine). Les sphères
-- qui couvrent l'outil sont tirées de ses formes (GetToolBody), une fois par outil.
-- ---------------------------------------------------------------------------
local pushTool, pushPts, pushPrev = nil, nil, nil
local PUSH_FORCE = 1500       -- N : force maximale d'un bras qui pousse
local PUSH_HIT = 25           -- N.s : en plus, pour un geste rapide (coup)
local PUSH_RESOLVE = 10       -- 1/s : vitesse à laquelle l'objet sort de l'outil (enfoncement)

local function toolSpheres(body)
	local pts = {}
	for _, sh in ipairs(GetBodyShapes(body) or {}) do
		local sx, sy, sz, scale = GetShapeSize(sh)
		if sx and sx > 0 and scale and scale > 0 then
			-- Position de la forme dans le corps de l'outil (pas via le monde : juste après
			-- un changement d'outil, la transformation monde du corps n'est pas encore valide).
			local st = GetShapeLocalTransform(sh, 1)   -- (1 : telle qu'elle est dessinée)
			local ext = { sx * scale, sy * scale, sz * scale }
			local long = 1
			for a = 2, 3 do if ext[a] > ext[long] then long = a end end
			local r = 0
			for a = 1, 3 do if a ~= long then r = math.max(r, ext[a] * 0.5) end end
			r = math.max(0.025, math.min(0.09, r))
			local n = math.max(1, math.ceil(ext[long] / (1.6 * r)))
			for i = 0, n - 1 do
				local c = { ext[1] * 0.5, ext[2] * 0.5, ext[3] * 0.5 }
				c[long] = ext[long] * (i + 0.5) / n
				pts[#pts + 1] = { TransformToParentPoint(st, Vec(c[1], c[2], c[3])), r }
				if #pts >= 12 then return pts end
			end
		end
	end
	return pts
end

local colPrev = nil       -- pose (monde) du corps de l'outil affichée à l'image précédente
local colOffset = nil     -- corps de l'outil par rapport à la main (mesuré)
local colLogT = 0
local colPrevWant = nil   -- pose voulue (main) de l'image précédente : vitesse réelle de la main
local colStuck = 0
local colHits = 0

local function lerpT(a, b, t)
	return Transform(VecLerp(a.pos, b.pos, t), QuatSlerp(a.rot, b.rot, t))
end

-- Fait glisser les sphères de l'outil de la pose a à la pose b (tout ce qui est
-- solide, sauf le joueur, l'outil, l'objet tenu et le véhicule). Renvoie la
-- fraction du trajet faite avant le premier contact, et ce contact.
local function sweepTool(a, b, grab, veh)
	local tmin, best = 1, nil
	for _, sp in ipairs(pushPts) do
		local p0 = TransformToParentPoint(a, sp[1])
		local p1 = TransformToParentPoint(b, sp[1])
		local d = VecSub(p1, p0)
		local len = VecLength(d)
		if len > 0.0002 then
			if grab ~= 0 then QueryRejectBody(grab) end
			if veh ~= 0 then QueryRejectVehicle(veh) end
			rejectPhys()   -- (ni le bras de la main libre)
			local dir = VecScale(d, 1 / len)
			local hit, dist, n, shape = QueryRaycast(p0, dir, len, sp[2])
			if hit and dist <= 0.0005 then
				-- Déjà au contact (ou un peu dedans) : l'outil peut ressortir, pas s'enfoncer.
				if VecLength(n) < 0.5 then
					if grab ~= 0 then QueryRejectBody(grab) end
					if veh ~= 0 then QueryRejectVehicle(veh) end
					rejectPhys()
					local h2, pt, n2, sh2 = QueryClosestPoint(p0, sp[2] + 0.03)
					if h2 then n, shape = n2, sh2 else hit = false end
				end
				if hit and VecDot(dir, n) < -0.05 then dist = 0 else hit = false end
			end
			if hit and dist / len < tmin then
				tmin = dist / len
				best = { n = n, shape = shape, d = d, t = tmin, sp = sp[1],
					p = VecSub(VecAdd(p0, VecScale(d, tmin)), VecScale(n, sp[2])) }
				if camDebug and dist == 0 then colStuck = (colStuck or 0) + 1 end
			end
		end
	end
	return tmin, best
end

-- Contact avec un objet mobile : il est poussé pour suivre la main (force d'un bras).
local function pushContact(c, dt)
	if dt < 0.003 then return false end   -- image de durée quasi nulle : pas de vitesse mesurable
	local bd = GetShapeBody(c.shape)
	if bd == 0 or bd == toolPushedBody or not IsBodyDynamic(bd) then return false end
	local pushDir = VecScale(c.n, -1)                         -- dans l'objet
	-- L'objet suit la vitesse réelle de la main (pas tout le retard accumulé quand
	-- l'outil était retenu : il était projeté), plus une petite correction de
	-- l'enfoncement de la main dans l'objet.
	local v = c.vh or VecScale(c.d, 1 / dt)
	local depth = math.max(0, (1 - c.t) * VecDot(c.d, pushDir))
	local want = math.max(0, VecDot(v, pushDir)) + math.min(depth, 0.1) * 5
	local vb = VecDot(GetBodyVelocityAtPos(bd, c.p), pushDir)
	local dv = want - vb
	if dv <= 0.02 then return true end
	local speed = VecLength(v)
	local cap = PUSH_FORCE * dt + PUSH_HIT * math.max(0, math.min(1, (speed - 1.5) / 4))
	local mass = GetBodyMass(bd)
	local imp = math.min(mass * dv, cap)
	ApplyBodyImpulse(bd, c.p, VecScale(pushDir, imp))
	if camDebug then
		debugMsg = string.format("tool push body %d mass %.1f want %.2f had %.2f imp %.2f", bd, mass, want, vb, imp)
	end
	return true
end

-- target : main (monde) où l'outil devrait être. Renvoie la main à donner au jeu
-- pour que l'outil ne traverse rien.
collideTool = function(st, dt, target)
	local body = GetToolBody()
	local tool = GetString("game.player.tool")
	if body == 0 then colPrev = nil return target end
	-- Le jeu change le corps de l'outil une image après son nom : on recalcule les
	-- sphères quand le nom, le corps ou ses formes changent.
	local shapes = GetBodyShapes(body) or {}
	local key = tool .. ":" .. body .. ":" .. #shapes .. ":" .. (shapes[1] or 0)
	key = key .. ":" .. modHold.ver   -- (prise d'un outil de mod changée : on remesure)
	key = key .. ":" .. (st.toolPivotKey or "")   -- (outil de mod réduit autour d'un autre point : idem)
	if key ~= pushTool then
		pushTool = key
		pushPts = toolSpheres(body)
		colPrev, colOffset, colPrevWant = nil, nil, nil
		if camDebug then
			local z0, z1 = 9, -9
			for _, sp in ipairs(pushPts) do z0 = math.min(z0, sp[1][3]); z1 = math.max(z1, sp[1][3]) end
			debugMsg = string.format("tool collision spheres for %s: %d, local z %.2f..%.2f", tool, #pushPts, z0, z1)
		end
	end
	-- Le corps de l'outil (image précédente) par rapport à la main donnée au jeu.
	-- (Juste après le changement d'outil, le jeu donne une orientation nulle : ignorée.)
	local function validRot(q) local l = q[1] * q[1] + q[2] * q[2] + q[3] * q[3] + q[4] * q[4] return l > 0.8 and l < 1.2 end
	local bt = GetBodyTransform(body)
	if lastShownT and validRot(lastShownT.rot) and validRot(bt.rot) then
		local o = TransformToLocalTransform(lastShownT, bt)
		-- (le corps d'un outil de mod réduit autour de sa bouche peut être à 2 ou 3 m de la main)
		if validRot(o.rot) and VecLength(o.pos) < 6 then
			-- Quand le joueur se déplace, le jeu dessine l'outil depuis une position du
			-- corps interpolée (entre deux pas de physique) : la mesure varie d'une image
			-- à l'autre et l'outil tremblait. Elle n'est donc affinée qu'à l'arrêt.
			if not colOffset then
				colOffset = o
			elseif VecLength(GetPlayerVelocity()) < 0.15 then
				colOffset = Transform(VecLerp(colOffset.pos, o.pos, 0.1), QuatSlerp(colOffset.rot, o.rot, 0.1))
			end
		end
	end
	if not colOffset or not pushPts or #pushPts == 0 then return target end
	if colPrev and not validRot(colPrev.rot) then colPrev = nil end
	local want = TransformToParentTransform(target, colOffset)
	local from = colPrev
	-- (saut mesuré à la main : l'origine du corps d'un outil de mod réduit peut être loin
	-- de la main, elle décrit alors un grand arc quand le poignet tourne)
	local handPt = TransformToLocalTransform(colOffset, Transform()).pos
	if not from or VecLength(VecSub(TransformToParentPoint(want, handPt), TransformToParentPoint(from, handPt))) > 1.0 then
		colPrev = want   -- recentrage, rotation par crans, téléportation : on repart de la main
		colPrevWant = want
		return target
	end
	local grab, veh = GetPlayerGrabBody(), GetPlayerVehicle()
	if not st.toolBlock then
		-- L'outil suit la main et traverse les obstacles ; il pousse ce qui bouge sur
		-- son passage (balayage le long du mouvement de la main).
		if colPrevWant then
			local tt, cc = sweepTool(colPrevWant, want, grab, veh)
			if cc then
				cc.vh = VecScale(VecSub(TransformToParentPoint(want, cc.sp), TransformToParentPoint(colPrevWant, cc.sp)), 1 / dt)
				pushContact(cc, dt)
			end
		end
		colPrev = want
		colPrevWant = want
		return target
	end
	local t, c = sweepTool(from, want, grab, veh)
	local function handVel(cc)
		if cc and colPrevWant then
			cc.vh = VecScale(VecSub(TransformToParentPoint(want, cc.sp), TransformToParentPoint(colPrevWant, cc.sp)), 1 / dt)
		end
	end
	handVel(c)
	local result = want
	local contacts = 0
	if c then
		contacts = 1
		colHits = colHits + 1
		pushContact(c, dt)
		-- On s'arrête à 1 cm de la surface (assez pour ne pas être pris pour « déjà dedans »).
		local stop = lerpT(from, want, math.max(0, t - 0.01 / math.max(VecLength(c.d), 0.01)))
		-- Le reste du mouvement glisse le long de la surface.
		local rest = VecSub(want.pos, stop.pos)
		rest = VecSub(rest, VecScale(c.n, VecDot(rest, c.n)))
		local slide = Transform(VecAdd(stop.pos, rest), want.rot)
		local t2, c2 = sweepTool(stop, slide, grab, veh)
		handVel(c2)
		if c2 then
			contacts = 2
			if GetShapeBody(c2.shape) ~= GetShapeBody(c.shape) then pushContact(c2, dt) end
			result = lerpT(stop, slide, math.max(0, t2 - 0.01 / math.max(VecLength(c2.d), 0.01)))
		else
			result = slide
		end
	end
	colPrev = result
	colPrevWant = want
	if camDebug then
		colLogT = colLogT + dt
		if colLogT > 1 and not debugMsg then
			colLogT = 0
			-- sphère la plus basse et sol sous elle
			local low, lowR, lowL = nil, 0, nil
			for _, sp in ipairs(pushPts) do
				local w = TransformToParentPoint(want, sp[1])
				if not low or w[2] < low[2] then low, lowR, lowL = w, sp[2], sp[1] end
			end
			local gh, gd = QueryRaycast(VecAdd(low, Vec(0, 2, 0)), Vec(0, -1, 0), 6, 0)
			local o = colOffset
			debugMsg = string.format("tool collision %s: contacts %d (total %d, held %d) gap %.3f | low y %.2f (local z %.2f) ground %s | offset %.2f %.2f %.2f q %.2f %.2f %.2f %.2f | body fwd y %.2f want fwd y %.2f",
				tool, contacts, colHits, colStuck, VecLength(VecSub(want.pos, result.pos)), low[2], lowL[3],
				gh and string.format("%.2f", low[2] + 2 - gd) or "none", o.pos[1], o.pos[2], o.pos[3], o.rot[1], o.rot[2],
				o.rot[3], o.rot[4], QuatRotateVec(bt.rot, Vec(0, 0, -1))[2], QuatRotateVec(want.rot, Vec(0, 0, -1))[2])
		end
	end
	return TransformToParentTransform(result, TransformToLocalTransform(colOffset, Transform()))
end

-- ---------------------------------------------------------------------------
-- Outil physique (comme Bonelabs). Une copie exacte de l'outil en main (mêmes
-- voxels, même place dans la main) est un vrai objet du jeu, tenu par la main
-- avec la force d'un bras (contrainte à impulsion limitée, résolue par le moteur
-- physique avec les contacts) : il ne traverse rien, glisse sur les surfaces,
-- pousse ce qui bouge (un objet lourd résiste et l'outil se décale dans la main).
-- Par défaut (PHYS_SHOW_COPY = false) la copie est invisible : l'outil du jeu est
-- montré à la main, décalé de l'écart physique de la copie, et les tirs et les effets
-- en partent. Le joueur ignore la copie (couche de collision à part).
-- ---------------------------------------------------------------------------
local PHYS_LAYER = 128     -- couche de collision de la copie
local PHYS_FORCE = 2000    -- N : force du bras sur l'outil
local PHYS_TORQUE = 120    -- N.m : couple du poignet
local PHYS_MAXVEL = 25     -- m/s
local PHYS_ANGVEL = 40     -- rad/s
-- Affichage : false = l'outil du jeu est montré à la main, décalé de l'écart physique
-- de la copie (contacts, inertie) mesuré au même instant de la simulation ; la copie
-- elle-même est invisible. Rien ne tremble alors quand le nombre de pas de physique
-- par image varie (images lentes). true = la copie est montrée telle que le moteur la
-- dessine (version précédente).
local PHYS_SHOW_COPY = false
local PHYS_HIT = 0.8       -- m/s : choc (vitesse de la copie changée net par un contact)
local PHYS_PRESS = 1.5     -- m/s : appui continu contre un obstacle (main ~3 cm dedans)
local PHYS_SNAP = 0.7      -- m : coincé plus loin que ça de la main, l'outil y revient

-- Résultat du dernier pas de physique : la vitesse obtenue par la copie comparée à
-- celle que le bras pouvait lui donner (consigne de update(), bornée par la force du
-- bras). Sans contact, l'écart est presque nul quelle que soit la cadence ; seul un
-- obstacle (ou un objet qui heurte l'outil) le crée.
local function physResidual()
	if not phys.pending or phys.body == 0 or not phys.cmdV or not phys.preV then return end
	phys.pending = false
	local bt = GetBodyTransform(phys.body)
	local v = GetBodyVelocityAtPos(phys.body, TransformToParentPoint(bt, GetBodyCenterOfMass(phys.body)))
	local lim = PHYS_FORCE / 60 / math.max(phys.mass or 1, 0.1)   -- vitesse que le bras donne en un pas
	local e = Vec(0, 0, 0)
	for i = 1, 3 do
		e[i] = v[i] - (phys.preV[i] + clamp(phys.cmdV[i] - phys.preV[i], -lim, lim))
	end
	local r = VecLength(e)
	phys.resMax = math.max(phys.resMax or 0, r)
	phys.resRise = math.max(phys.resRise or 0, r - (phys.lastRes or 0))
	phys.lastRes = r
end

-- Un autre objet (ni la copie, ni l'outil du jeu) qui collisionne avec la couche de la
-- copie est-il à moins de 3 cm de sa boîte englobante ?
local function physNearOther(toolBody)
	local mn, mx = GetBodyBounds(phys.body)
	local m = Vec(0.03, 0.03, 0.03)
	for _, s in ipairs(QueryAabbShapes(VecSub(mn, m), VecAdd(mx, m)) or {}) do
		local b = GetShapeBody(s)
		if b ~= phys.body and b ~= toolBody then
			local layer, mask = GetShapeCollisionFilter(s)
			if layer and layer > 0 and math.floor((mask or 0) / PHYS_LAYER) % 2 == 1 then return true end
		end
	end
	return false
end

local function validRot(q)
	local l = q[1] * q[1] + q[2] * q[2] + q[3] * q[3] + q[4] * q[4]
	return l > 0.8 and l < 1.2
end

physSetPlayerMask = function(ignore)
	local m = GetPlayerParam("CollisionMask")
	if type(m) ~= "number" then return end
	local has = math.floor(m / PHYS_LAYER) % 2 == 1
	if ignore and has then SetPlayerParam("CollisionMask", m - PHYS_LAYER)
	elseif not ignore and not has then SetPlayerParam("CollisionMask", m + PHYS_LAYER) end
end

physDestroy = function()
	if phys.body ~= 0 and IsHandleValid(phys.body) then Delete(phys.body) end
	for _, sh in ipairs(phys.hidden) do
		if IsHandleValid(sh) then RemoveTag(sh, "invisible") end
	end
	phys.body, phys.key, phys.offset, phys.want, phys.prevWant, phys.hidden, phys.stuckT = 0, nil, nil, nil, nil, {}, 0
	phys.shapes, phys.firing, phys.firingT, phys.hap, phys.hapT, phys.hapA = {}, false, 0, 0, 0, 0
	phys.rel, phys.ptick, phys.hrv, phys.targetAngVel, phys.goalEnd, phys.pending = nil, nil, nil, nil, nil, false
	phys.resMax, phys.resRise, phys.lastRes, phys.quietT, phys.lastStepP, phys.lastStepV = 0, 0, 0, 0, nil, nil
	phys.yaw, phys.recenter = nil, nil
end

-- Copies orphelines (sauvegarde rapide rechargée, script relancé) : supprimées.
local function physCleanup()
	for _, b in ipairs(FindBodies("tdvr_proxy", true) or {}) do
		if b ~= phys.body then Delete(b) end
	end
	-- (de même pour le bras de la main libre : voir tdvrOffArmTick)
	for _, b in ipairs(FindBodies("tdvr_arm", true) or {}) do
		if b ~= offArm.hand and b ~= offArm.arm then Delete(b) end
	end
	-- (et pour l'arme de l'autre main : voir tdvrDualTick)
	for _, b in ipairs(FindBodies("tdvr_dual", true) or {}) do
		if not tdvrDual or b ~= tdvrDual.copy then Delete(b) end
	end
end

-- Copie de la forme src (voxels de taille quelconque) en voxels de 10 cm dans dst
-- (créée avec la palette de l'outil : mêmes matériaux, donc une vraie masse).
-- Chaque case de 10 cm prend la matière de son premier voxel plein (ordre x, y, z) :
-- sa lecture s'arrête là. Le résultat est gardé par forme d'outil (key) : quand on
-- ressort le même outil, on ne relit que ces voxels-là pour vérifier. (Le revolver
-- faisait relire plus de 50 000 voxels à chaque sortie : une saccade.)
local function physDownsample(src, dst, key)
	local sx, sy, sz, scale = GetShapeSize(src)
	local f = math.max(1, math.floor(0.1 / scale + 0.5))
	local nx, ny, nz = math.ceil(sx / f), math.ceil(sy / f), math.ceil(sz / f)
	ResizeShape(dst, 0, 0, 0, nx - 1, ny - 1, nz - 1)
	phys.grids = phys.grids or {}
	if key then key = key .. ":" .. sx .. ":" .. sy .. ":" .. sz .. ":" .. f .. ":" .. GetShapeVoxelCount(src) end
	-- g : 7 nombres par case pleine (case cx cy cz, voxel lu x y z, matière)
	local g = key and phys.grids[key]
	if g then
		for i = 1, #g, 7 do
			local _, _, _, _, _, e = GetShapeMaterialAtIndex(src, g[i + 3], g[i + 4], g[i + 5])
			if e ~= g[i + 6] then g = nil break end   -- (forme changée : relue en entier)
		end
	end
	if not g then
		g = {}
		local n = 0
		for cx = 0, nx - 1 do
			local x0 = cx * f
			local x1 = math.min(sx, x0 + f) - 1
			for cy = 0, ny - 1 do
				local y0 = cy * f
				local y1 = math.min(sy, y0 + f) - 1
				for cz = 0, nz - 1 do
					local z0 = cz * f
					local z1 = math.min(sz, z0 + f) - 1
					local found = false
					for x = x0, x1 do
						for y = y0, y1 do
							for z = z0, z1 do
								local _, _, _, _, _, entry = GetShapeMaterialAtIndex(src, x, y, z)
								if entry and entry ~= 0 then
									g[n + 1], g[n + 2], g[n + 3], g[n + 4], g[n + 5], g[n + 6], g[n + 7] = cx, cy, cz, x, y, z, entry
									n = n + 7
									found = true
									break
								end
							end
							if found then break end
						end
						if found then break end
					end
				end
			end
		end
		if key and n <= 7 * 4096 then phys.grids[key] = g end
	end
	for i = 1, #g, 7 do
		SetBrush("cube", 1, g[i + 6])
		DrawShapeBox(dst, g[i], g[i + 1], g[i + 2], g[i], g[i + 1], g[i + 2])
	end
	return #g / 7
end

-- La copie : un corps mobile avec, pour chaque forme de l'outil,
--  - une forme visible, identique (mêmes voxels, même taille de voxel) mais sans
--    matière (ni masse ni collision), seulement si la copie peut être montrée
--    (PHYS_SHOW_COPY ou physShowReal) ;
--  - une forme physique invisible, la même en voxels de 10 cm (les formes créées
--    par un script ont des voxels de 10 cm), avec les matériaux de l'outil.
local function physBuild(toolBody, shapes, pose)
	physCleanup()
	-- (la petite boîte garde le corps mobile à sa création ; elle est retirée ensuite)
	local parts = { '<body tags="tdvr_proxy" dynamic="true"><voxbox size="1 1 1" material="wood" tags="tdvr_part=0"/>' }
	local list = {}
	for _, sh in ipairs(shapes) do
		local sx, sy, sz, scale = GetShapeSize(sh)
		if sx and sx > 0 and scale and scale > 0 and #list < 16 and GetShapeVoxelCount(sh) > 0 then
			list[#list + 1] = sh
			-- (forme visible : seulement si la copie peut être montrée ; sinon elle était créée,
			-- remplie puis cachée pour rien à chaque sortie d'outil - sans masse ni collision)
			if PHYS_SHOW_COPY or physShowReal then
				parts[#parts + 1] = string.format('<vox file="MOD/proxy.vox" scale="%.4f" tags="tdvr_part=%d"/>', scale * 10, #list)
			end
		end
	end
	if #list == 0 then return false end
	parts[#parts + 1] = '</body>'
	local ents = Spawn(table.concat(parts), pose, false, false)
	local b = 0
	local vis = {}
	for _, h in ipairs(ents or {}) do
		local ty = GetEntityType(h)
		if ty == "body" and b == 0 then
			b = h
		elseif ty == "shape" then
			local i = tonumber(GetTagValue(h, "tdvr_part"))
			if i then vis[i] = h end
		end
	end
	if b == 0 then return false end
	local n, coarse = 0, 0
	local phShapes = {}
	for i, sh in ipairs(list) do
		local lt = GetShapeLocalTransform(sh, 1)   -- (1 : telle qu'elle est dessinée)
		local v = vis[i]
		if v then
			CopyShapeContent(sh, v)
			CopyShapePalette(sh, v)
			SetShapeLocalTransform(v, lt)
			SetShapeCollisionFilter(v, 0, 0)     -- jamais de collision (affichage seulement)
			SetTag(v, "unbreakable")
			if not PHYS_SHOW_COPY and not physShowReal then SetTag(v, "invisible") end
		end
		local ph = CreateShape(b, lt, sh)
		if ph and ph ~= 0 then
			coarse = coarse + physDownsample(sh, ph, GetString("game.player.tool") .. "#" .. i)
			SetShapeLocalTransform(ph, lt)
			SetShapeCollisionFilter(ph, PHYS_LAYER, 255)
			SetTag(ph, "invisible")
			SetTag(ph, "unbreakable")
			phShapes[#phShapes + 1] = ph
			n = n + 1
		end
	end
	if vis[0] then Delete(vis[0]) end
	if n == 0 then Delete(b) return false end
	-- Poids d'un vrai outil : 6 kg pour la masse, 3 kg pour le reste.
	local m0 = GetBodyMass(b)
	local wantMass = GetString("game.player.tool") == "sledge" and 6 or 3
	-- (SetShapeDensity multiplie la densité des matériaux de la forme)
	if m0 > 0.01 then
		for _, ph in ipairs(phShapes) do SetShapeDensity(ph, wantMass / m0) end
	end
	phys.shapes = phShapes
	phys.body = b
	phys.mass = GetBodyMass(b)
	SetBodyActive(b, true)
	SetBodyVelocity(b, GetPlayerVelocity())
	physSetPlayerMask(true)
	if camDebug then
		physMsg = string.format("phys tool %s: body %d, %d shapes (%d coarse voxels), mass %.1f kg (before %.1f), dynamic %s",
			GetString("game.player.tool"), b, n, coarse, phys.mass, m0, tostring(IsBodyDynamic(b)))
		local pbs = GetPlayerBodies() or {}
		local pl, pm = -1, -1
		for _, pb in ipairs(pbs) do
			local s = (GetBodyShapes(pb) or {})[1]
			if s then
				pl, pm = GetShapeCollisionFilter(s)
				break
			end
		end
		-- (corps du personnage : s'ils collisionnaient avec la couche 128, la copie frotterait contre le joueur)
		physMsg = physMsg .. string.format(", copy shown %s, player bodies %d (layer %d mask %d)",
			tostring(PHYS_SHOW_COPY or physShowReal), #pbs, pl or -1, pm or -1)
	end
	return true
end

-- target : main (monde) où l'outil devrait être. Renvoie la main à donner au jeu
-- (l'outil du jeu se place à la main, décalé de l'écart physique de la copie).
-- Le personnage du joueur est fait de corps animés (invisibles en VR) dont les
-- formes touchaient toutes les couches, donc la copie aussi : l'outil frottait
-- contre le corps du joueur (bloqué à 20-30 cm de la main, « ça frotte dans
-- l'air »). Ces formes ignorent désormais la couche de la copie.
local function physPlayerFilter(dt)
	-- (l'outil physique et la main physique l'appellent tous deux : le délai est alors décompté
	-- deux fois, vérifié toutes les 0,5 s ; gardé tel quel, une seule recherche par image de toute façon)
	phys.bodyT = (phys.bodyT or 0) - dt
	if phys.bodyT > 0 then return end
	phys.bodyT = 1.0   -- (le jeu peut recréer le personnage : vérifié chaque seconde)
	for _, pb in ipairs(GetPlayerBodies() or {}) do
		for _, sh in ipairs(GetBodyShapes(pb) or {}) do
			local l, m = GetShapeCollisionFilter(sh)
			if m and math.floor(m / PHYS_LAYER) % 2 == 1 then SetShapeCollisionFilter(sh, l, m - PHYS_LAYER) end
		end
	end
end

physTool = function(st, dt, target)
	local body = GetToolBody()
	local tool = GetString("game.player.tool")
	if body == 0 or tool == "" or tool == "none" then
		if phys.body ~= 0 then physDestroy() end
		return target
	end
	local shapes
	if fastLua.on then
		-- (fastLua : les formes d'un gros outil - THE EVERYTHING REVOLVER, 1028 formes : une table de
		-- 1028 entrées à chaque image - gardées 10 images ; relues aussitôt si l'outil, son corps, sa
		-- prise ou sa taille changent, ou si la copie est à (re)construire. Un changement de ses
		-- formes seules est donc vu jusqu'à 10 images plus tard : la copie invisible est refaite alors.)
		local c, ver, piv = fastLua.tc, modHold.ver, st.toolPivotKey or ""
		if c.body ~= body or c.tool ~= tool or c.ver ~= ver or c.piv ~= piv or c.age >= 10 or #c.list <= 64
			or phys.body == 0 or not IsHandleValid(phys.body) then
			c.list = GetBodyShapes(body) or {}
			c.body, c.tool, c.ver, c.piv, c.age = body, tool, ver, piv, 0
		end
		c.age = c.age + 1
		shapes = c.list
	else
		shapes = GetBodyShapes(body) or {}
	end
	-- Copie refaite si l'outil, son corps ou ses formes changent, ou la prise ou la taille
	-- d'un outil de mod, ou le point autour duquel la DLL le réduit. (Comparés un par un :
	-- le texte de la clé n'est construit qu'au changement, pas à chaque image.)
	local kp, first, ver, piv = phys.keyParts, shapes[1] or 0, modHold.ver, st.toolPivotKey or ""
	if not phys.key or kp[1] ~= tool or kp[2] ~= body or kp[3] ~= #shapes or kp[4] ~= first or kp[5] ~= ver or kp[6] ~= piv then
		physDestroy()
		phys.key = tool .. ":" .. body .. ":" .. #shapes .. ":" .. first .. ":" .. ver .. ":" .. piv
		phys.keyParts = { tool, body, #shapes, first, ver, piv }
	end
	-- Place du corps de l'outil par rapport à la main donnée au jeu (mesurée sur
	-- l'image précédente ; affinée seulement à l'arrêt, le jeu dessine l'outil depuis
	-- une position du corps interpolée quand on marche).
	local bt = GetBodyTransform(body)
	if lastShownT and validRot(lastShownT.rot) and validRot(bt.rot) then
		local o = TransformToLocalTransform(lastShownT, bt)
		-- (le corps d'un outil de mod réduit autour de sa bouche peut être à 2 ou 3 m de la main)
		if validRot(o.rot) and VecLength(o.pos) < 6 then
			if not phys.offset then
				phys.offset = o
			elseif VecLength(GetPlayerVelocity()) < 0.15 then
				phys.offset = Transform(VecLerp(phys.offset.pos, o.pos, 0.1), QuatSlerp(phys.offset.rot, o.rot, 0.1))
			end
		end
	end
	if not phys.offset then return target end
	local want = TransformToParentTransform(target, phys.offset)
	if phys.body ~= 0 and not IsHandleValid(phys.body) then phys.body = 0 end
	if phys.body == 0 then
		if #shapes == 0 or not physBuild(body, shapes, want) then return target end
	end
	-- L'outil du jeu est caché seulement si la copie est montrée (PHYS_SHOW_COPY) ;
	-- sinon c'est lui qu'on voit, à la main + l'écart physique de la copie.
	if PHYS_SHOW_COPY and not physShowReal then
		for _, sh in ipairs(shapes) do
			if not HasTag(sh, "invisible") then
				SetTag(sh, "invisible")
				phys.hidden[#phys.hidden + 1] = sh
			end
		end
	end
	-- Mouvement de la main PAR RAPPORT AU JOUEUR (manette, rotation de la pièce), lissé.
	-- Le déplacement du joueur, lui, est suivi pas à pas dans update() (position et
	-- vitesse réelles du joueur à chaque pas de physique). Avant, la vitesse de la main
	-- (joueur compris) était l'écart d'une image divisé par sa durée ; or à 15-20 images/s
	-- le moteur ne fait que 2 pas (33 ms) par image de ~55 ms : vitesse sous-estimée de
	-- 40 %, outil ~10 cm en retard en marchant, et cet écart faisait vibrer la manette.
	local pp = GetPlayerTransform().pos
	local rel = VecSub(want.pos, pp)
	-- (saut détecté à la vitesse de la main : l'origine du corps d'un outil de mod réduit
	-- peut être à 2 m de la main, elle va très vite dès que le poignet tourne)
	local relHand = VecSub(target.pos, pp)
	-- Rotation par crans ou recentrage : la main saute d'un coup. À 15-20 images/s sa
	-- vitesse apparente reste sous le seuil de 12 m/s : on regarde donc aussi le lacet de
	-- la pièce et le compteur de recentrage. L'outil revient alors dans la main.
	local jump = phys.yaw ~= nil and ((st.turnMode == 0 and vrYaw ~= phys.yaw) or st.recenter ~= phys.recenter)
	phys.yaw, phys.recenter = vrYaw, st.recenter
	if phys.rel and phys.want and dt > 0.003 then
		local hv = VecScale(VecSub(rel, phys.rel), 1 / dt)
		local av = VecScale(quatToRotVec(QuatRotateQuat(want.rot, quatConj(phys.want.rot))), 1 / dt)
		if jump or VecLength(VecScale(VecSub(relHand, phys.relHand or relHand), 1 / dt)) > 12 then
			jump = true   -- pas de vitesse héritée du saut (sinon l'outil continue sur sa lancée)
			phys.hrv, phys.targetAngVel = Vec(0, 0, 0), Vec(0, 0, 0)
		else
			phys.hrv = VecLerp(phys.hrv or hv, hv, 0.5)
			if VecLength(av) > 30 then av = Vec(0, 0, 0) end
			phys.targetAngVel = VecLerp(phys.targetAngVel or av, av, 0.5)
		end
	elseif not phys.rel then
		phys.hrv = Vec(0, 0, 0)
		phys.targetAngVel = Vec(0, 0, 0)
	end
	phys.rel = rel
	phys.relHand = relHand
	phys.ptick = pp
	phys.prevWant = phys.want
	phys.want = want
	phys.target = target
	phys.stepsSum = (phys.stepsSum or 0) + (phys.steps or 0)
	phys.steps = 0
	-- Les rayons du jeu (tirs, coup de la gâchette, chalumeau...) partent de l'œil ou de
	-- l'outil : ils ignorent l'outil du jeu, mais pas la copie. Gâchette tenue, la copie
	-- ne touche donc plus rien (couche 0) le temps de l'utilisation.
	-- (de même pendant qu'on tient un objet : il ne doit pas buter contre l'outil)
	-- (objet porté : encore 0,3 s après l'avoir lâché, un objet lancé a le temps de
	-- s'éloigner de la main ; la gâchette, elle, rend les collisions tout de suite)
	-- La gâchette tire à 55 % de sa course (DLL) ; le jeu traite le tir avant ce script :
	-- la copie devient transparente dès 25 % de course, avant le tir (sinon le premier
	-- tir touchait la copie du fusil, qui sautait de 10-30 cm et arrêtait les plombs).
	physPlayerFilter(dt)
	local trig = st.leftHanded == 1 and st.ltrig or st.rtrig
	-- Le bouton « attraper » du jeu ne doit jamais saisir la copie elle-même (c'est un
	-- objet du jeu) : le jeu la tirerait d'un côté et la main de l'autre. Pour qu'il ne la
	-- choisisse pas (il la prenait puis ce script la lâchait aussitôt : l'attrape du grip
	-- droit durait une image), la copie est aussi transparente dès 25 % du grip (le jeu
	-- attrape à 60 %) quand elle est sur son rayon de sélection (3 m depuis la caméra du
	-- joueur, vers la cible du laser) : boîte de la copie touchée par ce rayon, 3 cm près.
	local grip = st.leftHanded == 1 and st.lsq or st.rsq
	local onPick = false
	if (grip or 0) > 0.25 then
		local pc = GetPlayerCameraTransform()
		local f = QuatRotateVec(pc.rot, Vec(0, 0, -1))
		local mn, mx = GetBodyBounds(phys.body)
		local t0, t1 = 0, 3 * (st.worldScale or 1)
		onPick = true
		for i = 1, 3 do
			local a, b = mn[i] - 0.03, mx[i] + 0.03
			if math.abs(f[i]) < 1e-6 then
				if pc.pos[i] < a or pc.pos[i] > b then onPick = false end
			else
				local u, v = (a - pc.pos[i]) / f[i], (b - pc.pos[i]) / f[i]
				if u > v then u, v = v, u end
				t0, t1 = math.max(t0, u), math.min(t1, v)
			end
		end
		onPick = onPick and t0 <= t1
	end
	if GetPlayerGrabBody() == phys.body then
		ReleasePlayerGrab()
		if camDebug then debugMsg = "physical tool copy: the game's grab took it - released" end
	end
	local grab = GetPlayerGrabBody() ~= 0
	phys.firingT = grab and 0.3 or math.max(0, (phys.firingT or 0) - dt)
	-- (coup de masse du jeu en cours : la copie reste hors de son chemin jusqu'à ce qu'il ait
	-- cherché ce qu'il frappe, gâchette lâchée ou non - voir updateTool)
	local firing = InputDown("usetool") or (trig or 0) > 0.25 or onPick or grab or phys.firingT > 0
		or (phys.strikeT or 0) > 0
	if firing ~= phys.firing then
		phys.firing = firing
		for _, ph in ipairs(phys.shapes or {}) do
			if IsHandleValid(ph) then SetShapeCollisionFilter(ph, firing and 0 or PHYS_LAYER, firing and 0 or 255) end
		end
		-- (collisions rendues : la copie peut ressortir d'un objet, ce n'est pas un choc)
		if not firing then phys.quietT = 0.15 end
	end
	local pt = GetBodyTransform(phys.body)
	-- (écart mesuré à la main : l'origine du corps d'un outil de mod réduit peut être à
	-- 2 m de la main, elle s'écarterait beaucoup au moindre retard en rotation)
	local handPt = TransformToLocalTransform(phys.offset, Transform()).pos
	local gap = VecLength(VecSub(TransformToParentPoint(pt, handPt), TransformToParentPoint(want, handPt)))
	if gap > PHYS_SNAP then phys.stuckT = phys.stuckT + dt else phys.stuckT = 0 end
	if jump or gap > 2.5 or phys.stuckT > 0.3 or not validRot(pt.rot) then
		-- Coincé loin de la main (ou téléportation) : l'outil revient dans la main.
		SetBodyTransform(phys.body, want)
		SetBodyVelocity(phys.body, GetPlayerVelocity())
		SetBodyAngularVelocity(phys.body, Vec(0, 0, 0))
		phys.stuckT = 0
		phys.goalEnd = want
		phys.pending = false
		phys.lastRes = 0
		phys.quietT = 0.15
		pt = want
	end
	-- Vibrations au contact : un choc (la copie freinée net) et une légère vibration
	-- tant que la main est retenue par un obstacle.
	-- Seulement quand la copie touche vraiment quelque chose : le moteur résout la
	-- consigne de vitesse de la copie (update) avec ses contacts ; sans contact, elle
	-- obtient la vitesse demandée, à toute cadence. L'écart (résidu) mesure donc la
	-- poussée d'un obstacle, et un saut du résidu est un choc. On vérifie aussi qu'un
	-- autre objet est bien à moins de 3 cm de la copie. (Plus de vibration liée à
	-- l'écart main-copie : à 15 images/s il venait du retard, pas d'un contact.)
	physResidual()   -- dernier pas de physique de l'image précédente
	local res, rise = phys.resMax or 0, phys.resRise or 0
	phys.resMax, phys.resRise = 0, 0
	phys.quietT = math.max(0, (phys.quietT or 0) - dt)
	local hap = 0
	local near = false
	if (rise > PHYS_HIT or res > PHYS_PRESS) and phys.quietT == 0 and not firing then
		near = physNearOther(body)
		if near then
			if rise > PHYS_HIT then
				-- choc : impulsion de durée fixe (même ressenti à 15 ou à 90 images/s)
				phys.hapA = math.max((phys.hapT or 0) > 0 and (phys.hapA or 0) or 0, math.min(1, 0.2 + rise / 5))
				phys.hapT = 0.07
			end
			-- appui contre un obstacle : vibration légère, plus forte si on appuie fort
			if res > PHYS_PRESS then hap = math.min(0.25, 0.06 + (res - PHYS_PRESS) * 0.04) end
		end
	end
	if (phys.hapT or 0) > 0 then
		hap = math.max(hap, phys.hapA or 0)
		phys.hapT = phys.hapT - dt
	end
	phys.hap = hap
	-- Écart physique de la copie : sa pose à la fin du dernier pas de physique comparée à
	-- la pose voulue au même instant de la simulation (update). Sans contact il est
	-- presque nul, à toute cadence ; contre un obstacle, c'est le recul de l'outil.
	local devPos, devRot = Vec(0, 0, 0), Quat()
	if phys.goalEnd then
		devPos = VecSub(pt.pos, phys.goalEnd.pos)
		devRot = QuatRotateQuat(pt.rot, quatConj(phys.goalEnd.rot))
	end
	if camDebug then
		phys.logT = phys.logT + dt
		local q = pt.rot
		local angGap = math.deg(VecLength(quatToRotVec(QuatRotateQuat(want.rot, Quat(-q[1], -q[2], -q[3], q[4])))))
		phys.maxGap = math.max(phys.maxGap or 0, gap)
		phys.maxAng = math.max(phys.maxAng or 0, angGap)
		phys.maxW = math.max(phys.maxW or 0, VecLength(phys.targetAngVel or Vec()))
		phys.maxDev = math.max(phys.maxDev or 0, VecLength(devPos))
		phys.maxRes = math.max(phys.maxRes or 0, res)
		phys.maxRise = math.max(phys.maxRise or 0, rise)
		phys.maxHap = math.max(phys.maxHap or 0, hap)
		phys.nearN = (phys.nearN or 0) + (near and 1 or 0)
		phys.ticksN = (phys.ticksN or 0) + 1
		-- main immobile dans la pièce (casque simulé) : doit rester ~0 en marchant, sinon la
		-- caméra (main) et GetPlayerTransform (pieds) ne sont pas au même instant
		phys.maxHrv = math.max(phys.maxHrv or 0, VecLength(phys.hrv or Vec()))
		-- le joueur est-il debout sur la copie ? (il ne doit pas : couche ignorée)
		local gc, gs = GetPlayerGroundContact()
		if gc and gs and gs ~= 0 and GetShapeBody(gs) == phys.body then phys.onCopy = (phys.onCopy or 0) + 1 end
		phys.logT2 = (phys.logT2 or 0.5) + dt
		if phys.logT2 > 1 and not debugMsg then
			phys.logT2 = 0
			debugMsg = string.format("phys contact %s: shown dev max %.3f m, residual max %.2f rise %.2f m/s, near %d/%d, hap max %.2f, player steps ok %d/%d, steps/frame %.2f, player %.2f m/s, on copy %d, hand rel speed max %.2f m/s, player lead %.2f steps",
				tool, phys.maxDev, phys.maxRes, phys.maxRise, phys.nearN, phys.ticksN, phys.maxHap,
				(phys.dbgStepN or 0) - (phys.dbgStepBad or 0), phys.dbgStepN or 0, (phys.stepsSum or 0) / math.max(phys.ticksN, 1),
				VecLength(GetPlayerVelocity()), phys.onCopy or 0, phys.maxHrv,
				(phys.dbgLead or 0) / math.max(phys.dbgLeadN or 0, 1))
			phys.maxDev, phys.maxRes, phys.maxRise, phys.maxHap, phys.nearN, phys.ticksN = 0, 0, 0, 0, 0, 0
			phys.onCopy, phys.stepsSum, phys.dbgStepN, phys.dbgStepBad = 0, 0, 0, 0
			phys.maxHrv, phys.dbgLead, phys.dbgLeadN = 0, 0, 0
		end
		if phys.logT > 1 and not debugMsg then
			phys.logT = 0
			local tb = GetBodyTransform(body)
			debugMsg = string.format("phys tool %s: gap %.3f m %.1f deg (max over 1 s %.3f m %.1f deg, hand turning up to %.1f rad/s), speed %.2f m/s, mass %.1f kg (dyn %s active %s), real tool vs copy %.3f m, hidden %s, player mask %s",
				tool, gap, angGap, phys.maxGap, phys.maxAng, phys.maxW, VecLength(GetBodyVelocity(phys.body)), GetBodyMass(phys.body), tostring(IsBodyDynamic(phys.body)),
				tostring(IsBodyActive(phys.body)), VecLength(VecSub(tb.pos, pt.pos)), tostring(shapes[1] and HasTag(shapes[1], "invisible")),
				tostring(GetPlayerParam("CollisionMask")))
			phys.maxGap, phys.maxAng, phys.maxW = 0, 0, 0
		end
	end
	if physShowReal then return target end   -- diagnostic : l'outil du jeu reste visible, dans la main
	local toHand = TransformToLocalTransform(phys.offset, Transform())
	if PHYS_SHOW_COPY then return TransformToParentTransform(pt, toHand) end
	-- L'outil du jeu, montré à la main (repère du rendu) + l'écart physique de la copie.
	local shownBody = Transform(VecAdd(want.pos, devPos), QuatRotateQuat(devRot, want.rot))
	return TransformToParentTransform(shownBody, toHand)
end

-- Essai sur le casque simulé uniquement : gâchette gauche = une caisse de test
-- devant le joueur (au sol), suivie dans le journal.
local mockCrate = { h = 0, prevTrig = 0, t = 0, p0 = nil }
-- Casque simulé seulement : images lentes (boucle de calcul dans le tick) pour essayer
-- l'outil physique à 15-20 images/s. 0 = coupé ; ~3000000 donne environ 40 ms.
local MOCK_SLOW = 0
local function mockPushTest(st, dt)
	if math.floor(st.debugFlags / 32) % 2 ~= 1 then return end
	local bBtn = math.floor(st.buttons / 2) % 2 == 1
	-- (armes doubles, tdvrDualTick : la gâchette gauche tire et le grip gauche fait s'accroupir -
	-- pas d'essais sur ces boutons pendant ce temps)
	local dual = tdvrDual and tdvrDual.on
	-- B + gâchette gauche : scène d'essai de la main physique (tdvrMockOffHandScene, plus bas).
	if dual then
		-- (rien)
	elseif st.ltrig > 0.5 and mockCrate.prevTrig <= 0.5 and bBtn then
		tdvrMockOffHandScene()
	elseif st.ltrig > 0.5 and mockCrate.prevTrig <= 0.5 and not offHand.panel then
		-- (pas quand le laser gauche vise un panneau : sa gâchette y choisit)
		if mockCrate.h ~= 0 and IsHandleValid(mockCrate.h) then Delete(mockCrate.h) end
		local pt = GetPlayerTransform()
		local cf = QuatRotateVec(GetCameraTransform(1).rot, Vec(0, 0, -1))   -- (1 : vraie caméra, la tête)
		local fwd = VecNormalize(Vec(cf[1], 0, cf[3]))
		-- devant la tête de la masse (centre de la caisse), posée au sol
		local base = phys.target and TransformToParentPoint(TransformToParentTransform(phys.target, SLEDGE_GRIP), HEAD[2]) or pt.pos
		local c = VecAdd(Vec(base[1], pt.pos[2] + 0.05, base[3]), VecScale(fwd, 0.6))
		local p = TransformToParentPoint(Transform(c, pt.rot), Vec(-0.25, 0, 0.25))
		local ents = Spawn('<body dynamic="true" tags="tdvr_testcrate"><voxbox size="5 5 5" material="wood"/></body>', Transform(p, pt.rot), false, false)
		for _, h in ipairs(ents or {}) do if GetEntityType(h) == "body" then mockCrate.h = h end end
		mockCrate.p0 = p
		mockCrate.t = 0
	end
	mockCrate.prevTrig = st.ltrig
	if MOCK_SLOW > 0 then
		local x = 0
		for i = 1, MOCK_SLOW do x = x + i % 7 end
		mockCrate.sink = x
	end
	-- B + grip gauche ensemble : monter dans le véhicule le plus proche / en sortir (essai de conduite)
	if st.lsq > 0.5 and bBtn and not mockCrate.vehReq then
		mockCrate.vehReq = true
		local best, bd = 0, 1e9
		local pp = GetPlayerTransform().pos
		for _, v in ipairs(FindVehicles("", true) or {}) do
			local d = VecLength(VecSub(GetVehicleTransform(v).pos, pp))
			if d < bd then best, bd = v, d end
		end
		if GetPlayerVehicle() ~= 0 then
			SetPlayerVehicle(0)   -- sortie forcée (comme en tombant dans l'eau)
			debugMsg = "mock: forced out of the vehicle"
		else
			if best ~= 0 then SetPlayerVehicle(best) end
			debugMsg = string.format("mock: entering vehicle %d at %.1f m", best, bd)
		end
	elseif st.lsq < 0.5 and not bBtn then
		mockCrate.vehReq = false
	end
	local veh = GetPlayerVehicle()
	if veh ~= 0 then
		mockCrate.vt = (mockCrate.vt or 0) + dt
		if mockCrate.vt > 0.5 and not debugMsg then
			mockCrate.vt = 0
			local vb = GetVehicleBody(veh)
			local v = GetBodyVelocity(vb)
			local f = QuatRotateVec(GetVehicleTransform(veh).rot, Vec(0, 0, -1))
			debugMsg = string.format("mock vehicle: speed %.2f m/s (forward %.2f), yaw rate %.2f rad/s, vehicle yaw %.1f, view yaw %.1f, triggers %.2f %.2f",
				VecLength(v), VecDot(v, f), GetBodyAngularVelocity(vb)[2], yawOf(GetVehicleTransform(veh).rot), vrYaw, st.rtrig, st.ltrig)
		end
	end
	if veh == 0 then
		mockCrate.wt = (mockCrate.wt or 0) + dt
		if mockCrate.wt > 0.5 and not debugMsg then
			mockCrate.wt = 0
			debugMsg = string.format("mock walk: player speed %.2f m/s, game keys up=%s down=%s left=%s right=%s, stick %.2f %.2f, crouch %.2f, triggers %.2f %.2f, grips %.2f %.2f",
				VecLength(GetPlayerVelocity()), tostring(InputDown("up")), tostring(InputDown("down")), tostring(InputDown("left")),
				tostring(InputDown("right")), st.lsx, st.lsy, GetPlayerCrouch(), st.rtrig, st.ltrig, st.rsq, st.lsq)
		end
	end
	-- grip gauche seul, appui « s'accroupir » (rien à attraper sous la main : voir input.cpp) :
	-- une explosion à 7 m devant (mesure des saccades). Jamais pendant les essais de la main
	-- physique (scène de tdvrMockOffHandScene).
	local crouchPress = offHand.press == 1
	if crouchPress and not mockCrate.prevCrouch and not bBtn and st.ltrig < 0.5 and not mockCrate.lh and not dual then
		local pt = GetPlayerTransform()
		local cf = QuatRotateVec(GetCameraTransform(1).rot, Vec(0, 0, -1))   -- (1 : vraie caméra, la tête)
		local fwd = VecNormalize(Vec(cf[1], 0, cf[3]))
		Explosion(VecAdd(pt.pos, VecAdd(VecScale(fwd, 7), Vec(0, 0.5, 0))), 4)
	end
	mockCrate.prevCrouch = crouchPress
	-- Essais de la main physique : la caisse de la scène, suivie dans le journal.
	local lc = mockCrate.lh and mockCrate.lh[2] or 0
	if lc ~= 0 and IsHandleValid(lc) and mockCrate.lhP0 then
		mockCrate.lhT = (mockCrate.lhT or 0) + dt
		if mockCrate.lhT > 0.5 and not debugMsg then
			mockCrate.lhT = 0
			local d = VecSub(GetBodyTransform(lc).pos, mockCrate.lhP0)
			local s0 = (GetBodyShapes(lc) or {})[1]
			local l, m = -1, -1
			if s0 then l, m = GetShapeCollisionFilter(s0) end
			debugMsg = string.format("mock off-hand crate moved %.2f %.2f %.2f speed %.2f mass %.1f held %s layer %s mask %s",
				d[1], d[2], d[3], VecLength(GetBodyVelocity(lc)), GetBodyMass(lc), tostring(offHand.held == lc),
				tostring(l), tostring(m))
		end
	end
	if mockCrate.h ~= 0 and IsHandleValid(mockCrate.h) then
		mockCrate.t = mockCrate.t + dt
		if mockCrate.t > 0.5 and not debugMsg then
			mockCrate.t = 0
			local bt = GetBodyTransform(mockCrate.h)
			local d = VecSub(bt.pos, mockCrate.p0)
			-- tête de la masse (copie physique) et main, dans le repère de la caisse
			local function headOf(bodyPose)
				local hand = TransformToParentTransform(bodyPose, TransformToLocalTransform(phys.offset, Transform()))
				return TransformToLocalPoint(bt, TransformToParentPoint(TransformToParentTransform(hand, SLEDGE_GRIP), HEAD[2]))
			end
			local hp = (phys.body ~= 0 and phys.offset) and headOf(GetBodyTransform(phys.body)) or Vec()
			local hw = (phys.want and phys.offset) and headOf(phys.want) or Vec()
			local mn, mx = GetBodyBounds(mockCrate.h)
			debugMsg = string.format("testcrate moved %.2f %.2f %.2f speed %.2f mass %.1f size %.2f %.2f %.2f | head %.2f %.2f %.2f want %.2f %.2f %.2f (crate frame)",
				d[1], d[2], d[3], VecLength(GetBodyVelocity(mockCrate.h)), GetBodyMass(mockCrate.h),
				mx[1] - mn[1], mx[2] - mn[2], mx[3] - mn[3], hp[1], hp[2], hp[3], hw[1], hw[2], hw[3])
		end
	end
end

-- Pas de physique (60 par seconde) : la main tire la copie vers sa place, avec
-- une force et un couple limités ; les contacts gagnent toujours. La vitesse
-- voulue = vitesse de la main (joueur compris) + rattrapage de l'écart.
local PHYS_GAIN = 0.8
local PHYS_SIGN = 1
local AXES = { Vec(1, 0, 0), Vec(0, 1, 0), Vec(0, 0, 1) }
function update(dt)
	-- Main libre physique (voir tdvrOffHandTick). (pcall : une erreur ici ne doit jamais
	-- arrêter l'outil physique, plus bas)
	pcall(tdvrOffHandStep, dt)
	if phys.body == 0 or not phys.want or dt <= 0 then return end
	if not IsHandleValid(phys.body) then phys.body = 0 return end
	SetBodyActive(phys.body, true)   -- un outil tenu ne s'endort jamais
	physResidual()                   -- résultat du pas précédent (contact ?)
	local bt = GetBodyTransform(phys.body)
	local c = GetBodyCenterOfMass(phys.body)
	local p = TransformToParentPoint(bt, c)
	-- Plusieurs pas de physique dans la même image (image lente, rattrapage) : chaque
	-- pas vise la main là où elle sera à ce moment (sinon l'outil traînait derrière).
	local k = phys.steps or 0
	phys.steps = k + 1
	-- Main au début de ce pas : main du tick + déplacement réel du joueur depuis (lu à
	-- chaque pas) + mouvement de la main par rapport au joueur, prolongé.
	local pp = GetPlayerTransform().pos
	local moved = phys.ptick and VecSub(pp, phys.ptick) or Vec(0, 0, 0)
	if VecLength(moved) > 1 then moved = Vec(0, 0, 0) end   -- téléportation : le tick suivant replace l'outil
	local hrv = phys.hrv or Vec(0, 0, 0)
	local tav = phys.targetAngVel or Vec(0, 0, 0)
	local want = phys.want
	local rot = want.rot
	local rv = VecScale(tav, dt * k)
	local a = VecLength(rv)
	if a > 1e-5 then rot = QuatRotateQuat(QuatAxisAngle(VecScale(rv, 1 / a), math.deg(a)), rot) end
	want = Transform(VecAdd(VecAdd(want.pos, moved), VecScale(hrv, dt * k)), rot)
	-- Vitesse de la main pendant ce pas : celle du joueur (que le moteur va intégrer)
	-- + celle de la main par rapport au joueur ; au centre de masse, la rotation de la
	-- main ajoute la sienne.
	local pv = GetPlayerVelocity()
	local goal = TransformToParentPoint(want, c)
	local tv = VecAdd(VecAdd(pv, hrv), VecCross(tav, VecSub(goal, want.pos)))
	local v = VecAdd(tv, VecScale(VecSub(goal, p), PHYS_GAIN / dt))
	local sp = VecLength(v)
	if sp > PHYS_MAXVEL then v = VecScale(v, PHYS_MAXVEL / sp) end
	local imp = PHYS_FORCE * dt
	for i = 1, 3 do
		ConstrainVelocity(phys.body, 0, p, AXES[i], PHYS_SIGN * v[i], -imp, imp)
	end
	-- Rotation : vitesse de rotation de la main + rattrapage de l'écart d'angle.
	local w = VecAdd(phys.targetAngVel or Vec(0, 0, 0), VecScale(quatToRotVec(QuatRotateQuat(want.rot, quatConj(bt.rot))), PHYS_GAIN / dt))
	local wl = VecLength(w)
	if wl > PHYS_ANGVEL then w = VecScale(w, PHYS_ANGVEL / wl) end
	local aimp = PHYS_TORQUE * dt
	for i = 1, 3 do
		ConstrainAngularVelocity(phys.body, 0, AXES[i], PHYS_SIGN * w[i], -aimp, aimp)
	end
	-- Pour la suite : pose voulue à la fin de ce pas (affichage, physTool), consigne et
	-- vitesse de départ (mesure du contact, physResidual).
	local re = VecScale(tav, dt)
	local ae = VecLength(re)
	local rotEnd = rot
	if ae > 1e-5 then rotEnd = QuatRotateQuat(QuatAxisAngle(VecScale(re, 1 / ae), math.deg(ae)), rot) end
	phys.goalEnd = Transform(VecAdd(want.pos, VecScale(VecAdd(pv, hrv), dt)), rotEnd)
	phys.cmdV, phys.preV, phys.pending = v, GetBodyVelocityAtPos(phys.body, p), true
	if camDebug then
		-- Le joueur avance-t-il bien à chaque pas (position lue ici) ? Pas « bon » si son
		-- déplacement depuis le pas précédent vaut sa vitesse x dt (à 50 % près).
		if phys.lastStepP and phys.lastStepV then
			local pred = VecLength(phys.lastStepV) * dt
			if pred > 0.001 then
				phys.dbgStepN = (phys.dbgStepN or 0) + 1
				if math.abs(VecLength(VecSub(pp, phys.lastStepP)) - pred) > 0.5 * pred then
					phys.dbgStepBad = (phys.dbgStepBad or 0) + 1
				end
			end
		end
		phys.lastStepP, phys.lastStepV = pp, pv
		-- Avance du joueur au moment de update(), en pas depuis le tick : 0 attendu (il
		-- avance pendant le pas) ; 1 = il a déjà avancé, la copie aurait un pas d'avance.
		local pvl = VecLength(pv)
		if pvl > 0.5 then
			phys.dbgLead = (phys.dbgLead or 0) + VecLength(moved) / (pvl * dt) - k
			phys.dbgLeadN = (phys.dbgLeadN or 0) + 1
		end
	end
end
-- ---------------------------------------------------------------------------
-- Bras de la main libre (gauche ; droite en mode gaucher), comme la main qui tient
-- l'outil : la main et l'avant-bras du personnage du jeu (ses voxels, sa taille)
-- suivent la manette. La main serre la poignée de la manette comme celle d'une arme
-- (repère « ik_hand_l » du personnage, celui que le jeu pose sur les poignées des
-- outils : +X devant, +Y le long du poing, +Z à droite). L'avant-bras va du poignet
-- vers un coude deviné (épaule sous la tête, bras de 30 cm), le poignet pliant de 45°
-- au plus. Deux corps fixes, sans collision ni dégâts, déplacés à chaque image normale ;
-- l'image de l'œil droit les garde où l'œil gauche les a vus. La DLL les recale au
-- rendu sur la caméra vraiment dessinée (le joueur avance pendant la physique, après
-- ce tick) : message TDVRE, la caméra de l'œil moins la main. Rien n'est demandé au
-- jeu : les outils qui posent eux-mêmes une main gauche (SetToolHandPoseLocalTransform,
-- vue à la troisième personne) ne sont pas touchés, et la main suit toujours la manette
-- (même près de la poignée avant d'une arme : l'arme ne suit que l'autre main).
-- Caché en véhicule, sur la carte, dans la vue de la caméra d'un mod loin du joueur,
-- mort, ou quand la manette n'est plus suivie. Réglage « Bras de la main libre ».
-- Appelé par tick() (fonction globale : tick() n'a plus de place pour un upvalue).
-- ---------------------------------------------------------------------------
-- Pièces du prefab du personnage (data/prefab/player/player_sven_lagom.xml) : objet
-- du .vox, position et rotation de la forme dans le repère de son os, poignée tenue
-- (ik_hand_*) dans le repère de la main, main dans le repère de l'avant-bras.
offArm.L = { hand = { "hand_l", "0.05 0.05 -0.05", "180 0 0" }, arm = { "arm_lower_l", "0.127 -0.014 -0.05", "90 0 5.1945" },
	ikInv = TransformToLocalTransform(Transform(Vec(0.075, 0.025, 0), QuatEuler(-90, 0, 0)), Transform()),
	wrist = Transform(Vec(0.276134, 0, 0), QuatEuler(-90, 0, 5.194495)) }
offArm.R = { hand = { "hand_r", "0.1 0.05 0.05", "180 180 0" }, arm = { "arm_lower_r", "0.127 -0.014 0.05", "90 180 -5.1945" },
	ikInv = TransformToLocalTransform(Transform(Vec(0.075, 0.025, 0), QuatEuler(90, 0, 0)), Transform()),
	wrist = Transform(Vec(0.276134, 0, 0), QuatEuler(90, 0, 5.194522)) }
offArm.L.wristInv = TransformToLocalTransform(offArm.L.wrist, Transform())
offArm.R.wristInv = TransformToLocalTransform(offArm.R.wrist, Transform())
-- Poignée OpenXR (« grip » : -Z dans le poing, +X à droite, pour les deux mains) -> repère ik_hand_*.
offArm.GRIP = Transform(Vec(0, 0, 0), Quat(-0.5, 0.5, -0.5, 0.5))
-- Personnages dont la main et l'avant-bras sont placés comme ceux de Sven ; les autres
-- prennent ceux de Sven (le personnage n'est pas dessiné à la première personne).
offArm.SAME = { sven_lagom = true, bjorn = true, carlos_lagom = true, fumiaki_lagom = true,
	james_thorvaldsson = true, magnus = true, nils_andree = true, spacesuit = true, chris_seabranch = true }

offArm.voxFile = function()
	local name = string.match(GetString("game.player.character.path") or "", "player_([%w_]+)%.xml$")
	if name and offArm.SAME[name] then return "player/player_" .. name .. ".vox" end
	return "player/player_sven_lagom.vox"
end

offArm.destroy = function()
	if offArm.hand ~= 0 and IsHandleValid(offArm.hand) then Delete(offArm.hand) end
	if offArm.arm ~= 0 and IsHandleValid(offArm.arm) then Delete(offArm.arm) end
	offArm.hand, offArm.arm, offArm.shapes, offArm.side, offArm.vox, offArm.shown = 0, 0, {}, nil, nil, false
end

-- Un corps fixe avec une forme du personnage, placée comme dans son prefab (t : l'os).
offArm.part = function(def, file, t)
	local xml = string.format('<body tags="tdvr_arm nomerge" dynamic="false"><vox tags="unbreakable nocull" pos="%s" rot="%s" '
		.. 'file="%s" object="%s" scale="0.5" collide="false"/></body>', def[2], def[3], file, def[1])
	local body, shape = 0, 0
	for _, h in ipairs(Spawn(xml, t, true, false) or {}) do
		local ty = GetEntityType(h)
		if ty == "body" and body == 0 then
			body = h
		elseif ty == "shape" and shape == 0 then
			shape = h
		end
	end
	if shape ~= 0 then SetShapeCollisionFilter(shape, 0, 0) end   -- (ne touche rien, rien ne le touche)
	return body, shape
end

offArm.spawn = function(side, file, handT, armT)
	offArm.destroy()
	for _, b in ipairs(FindBodies("tdvr_arm", true) or {}) do Delete(b) end   -- (orphelins : sauvegarde rechargée)
	local hs, as
	offArm.hand, hs = offArm.part(side.hand, file, handT)
	offArm.arm, as = offArm.part(side.arm, file, armT)
	offArm.shapes = { hs, as }
	offArm.side, offArm.vox, offArm.shown, offArm.sendT = side, file, true, 0
	return offArm.hand ~= 0 and offArm.arm ~= 0 and hs ~= 0 and as ~= 0
end

offArm.show = function(on)
	if on == offArm.shown then return end
	offArm.shown = on
	for _, s in ipairs(offArm.shapes) do
		if s ~= 0 and IsHandleValid(s) then
			if on then RemoveTag(s, "invisible") else SetTag(s, "invisible") end
		end
	end
end

-- allowed : à pied, ni carte ni caméra d'un mod loin du joueur (voir tick).
-- Diagnostic (debugLua) : ce que le jeu voit des boutons de l'outil et de l'attrape (la main
-- qui tient l'outil), son objet attrapé et son objet visé, à chaque changement (200 lignes au
-- plus). Comparé aux appuis de la DLL (« Input: dominant hand ... ») : qui relâche ?
function tdvrInputDiag()
	local d = tdvrInDiag
	if not d then
		d = { n = 0 }
		tdvrInDiag = d
	end
	local use, grab, gb = InputDown("usetool"), InputDown("grab"), GetPlayerGrabBody()
	if use == d.use and grab == d.grab and gb == d.gb then return end
	d.use, d.grab, d.gb = use, grab, gb
	d.n = d.n + 1
	if d.n > 200 then return end
	local what = ""
	if gb ~= 0 then
		what = gb == phys.body and " (tool copy)" or (gb == offHand.body and " (physical hand)"
			or string.format(" (%.1f kg)", IsHandleValid(gb) and GetBodyMass(gb) or 0))
	end
	local pb = GetPlayerPickBody()
	local m = string.format("input diag: game sees usetool %s grab %s, grab body %d%s, pick body %d%s, can use tool %s",
		tostring(use), tostring(grab), gb, what, pb, pb ~= 0 and pb == phys.body and " (tool copy)" or "",
		tostring(GetBool("game.player.canusetool")))
	pcall(SetClipboardText, "TDVRL " .. m)   -- (envoyé tout de suite : debugMsg peut être remplacé dans la même image)
end

function tdvrOffArmTick(st, toWorld, frozen, allowed)
	-- Laser de cette main (voir tdvrOffLaserTick) : avant tout le reste (sa cible sert à la main
	-- physique), dans les deux yeux. (pcall : une erreur du laser n'arrête pas le bras)
	local lok, lerr = pcall(tdvrOffLaserTick, st, toWorld, frozen, allowed)
	if not lok then
		offHand.lt = 0
		if camDebug and not debugMsg then debugMsg = "offlaser error: " .. tostring(lerr) end
	end
	if frozen then return end   -- (œil droit : le bras reste où l'œil gauche l'a vu)
	offHand.drawnIk = nil   -- (la main dessinée de cette image : l'arme de cette main la suit, voir tdvrDualTick)
	if camDebug then pcall(tdvrInputDiag) end
	-- (main physique, voir tdvrOffHandTick : collisions des objets lâchés. pcall partout : une
	-- erreur de la main physique ne doit arrêter ni le bras ni le reste de tick)
	pcall(offHand.restoreTick)
	local on, ph, gr = false, false, false
	local armFrom, armTo = 100, #tokens - 1
	if fastLua.tagsOk then
		local j = fastLua.tag("ARM", armFrom, armTo)   -- (fastLua : étiquette déjà repérée)
		armFrom, armTo = j or 1, j or 0
	end
	for j = armFrom, armTo do
		if tokens[j] == "ARM" then
			on = tokens[j + 1] == "1"
			-- main physique (comme Bonelabs), son grip attrape, appui du grip en cours (DLL :
			-- 0 relâché, 1 s'accroupir, 2 attraper ; décidé au moment où il s'enfonce)
			ph, gr = on and tokens[j + 2] == "1", on and tokens[j + 3] == "1"
			offHand.press = tonumber(tokens[j + 2]) and tonumber(tokens[j + 4]) or 0   -- (DLL plus ancienne : 0)
			-- (réglage grabKeepDistance et vitesse du joystick pour l'objet tenu de loin ; DLL plus
			-- ancienne : règles du jeu, joystick sans effet)
			offHand.keep, offHand.stickV = tokens[j + 5] == "1", tonumber(tokens[j + 6]) or 0
			break
		end
	end
	local right = st.leftHanded == 1   -- (la main libre est la droite en mode gaucher)
	local tracked = math.floor(st.buttons / (right and 256 or 128)) % 2 == 1
	local alive = GetPlayerHealth() > 0
	if not (on and allowed and tracked and alive) then
		-- (pourquoi le bras est caché : la raison d'un objet lâché, dans le journal)
		offHand.hideWhy = not on and "arm off" or (not allowed and "vehicle, map, mod camera or menu")
			or (not tracked and "controller not tracked") or "player dead"
		-- (main physique rangée tant que le bras est caché ; supprimée si elle est coupée)
		if ph then
			pcall(tdvrOffHandTick, st, nil, false)
		elseif offHand.body ~= 0 or offHand.held ~= 0 then
			pcall(offHand.destroy)
		end
		if not on then
			if offArm.hand ~= 0 then offArm.destroy() end
		else
			offArm.show(false)
		end
		return
	end
	local side = right and offArm.R or offArm.L
	-- La main : la poignée de la manette dans le repère ik de la main du personnage.
	local ik = TransformToParentTransform(toWorld(right and st.rgrip or st.lgrip), offArm.GRIP)
	-- Main physique : la main est dessinée là où est la main physique (contre un obstacle, sur
	-- l'objet qu'elle tient) ; l'avant-bras la suit (IK ci-dessous). (Dernier argument : la
	-- visée de l'autre main, l'axe de ses tirs.)
	if ph then
		local ok, shown = pcall(tdvrOffHandTick, st, ik, gr, toWorld(right and st.laim or st.raim))
		if ok then
			ik = shown or ik
		elseif camDebug and not debugMsg then
			debugMsg = "offhand error: " .. tostring(shown)
		end
	elseif offHand.body ~= 0 or offHand.held ~= 0 then
		pcall(offHand.destroy)
	end
	offHand.drawnIk = ik
	local hand = TransformToParentTransform(ik, side.ikInv)
	-- L'avant-bras : du poignet (origine de la main) vers le coude, par IK à deux segments
	-- depuis une épaule devinée sous la tête (repère du lacet de la tête).
	local rigid = TransformToParentTransform(hand, side.wristInv)   -- (poignet droit)
	local x0 = QuatRotateVec(rigid.rot, Vec(1, 0, 0))
	local head = toWorld(st.head)
	local s = st.worldScale
	local yawQ = QuatEuler(0, yawOf(head.rot), 0)
	local sx = right and 1 or -1
	local S = VecAdd(head.pos, QuatRotateVec(yawQ, Vec(sx * 0.17 * s, -0.25 * s, 0.06 * s)))
	local W = hand.pos
	local f = x0
	local d = VecLength(VecSub(W, S))
	if d > 0.05 then
		local U, L = 0.30 * s, side.wrist.pos[1]
		local n = VecScale(VecSub(W, S), 1 / d)
		local dd = clamp(d, math.abs(U - L) + 0.01, U + L - 0.001)
		local a = (U * U - L * L + dd * dd) / (2 * dd)
		local h = math.sqrt(math.max(0, U * U - a * a))
		local pole = QuatRotateVec(yawQ, Vec(sx * 0.5, -1, 0.25))   -- (coude en bas, vers l'extérieur, en arrière)
		local pp = VecSub(pole, VecScale(n, VecDot(pole, n)))
		if VecLength(pp) > 0.001 then pp = VecNormalize(pp) end
		local e = VecSub(W, VecAdd(VecAdd(S, VecScale(n, a)), VecScale(pp, h)))
		if VecLength(e) > 0.001 then f = VecNormalize(e) end
	end
	-- Le poignet plie de 45° au plus (au-delà, l'avant-bras suit la main).
	local armRot = rigid.rot
	local axis = VecCross(x0, f)
	if VecLength(axis) > 1e-5 then
		local ang = math.deg(math.acos(clamp(VecDot(x0, f), -1, 1)))
		armRot = QuatRotateQuat(QuatAxisAngle(VecNormalize(axis), math.min(ang, 45)), rigid.rot)
	end
	local arm = Transform(VecSub(W, QuatRotateVec(armRot, side.wrist.pos)), armRot)
	-- Corps créés (ou refaits : main changée, personnage changé, niveau rechargé).
	offArm.checkT = offArm.checkT - 1
	local file = offArm.vox
	if offArm.checkT <= 0 or not file then
		offArm.checkT = 60
		file = offArm.voxFile()
	end
	if offArm.hand == 0 or offArm.side ~= side or file ~= offArm.vox or not IsHandleValid(offArm.hand)
		or not IsHandleValid(offArm.arm) then
		-- (après un échec, nouvel essai 10 s plus tard : pas un Spawn à chaque image)
		if offArm.waitT > 0 then
			offArm.waitT = offArm.waitT - 1
			return
		end
		if not offArm.spawn(side, file, hand, arm) then
			offArm.destroy()
			offArm.waitT = 600
			if not debugMsg then debugMsg = "offarm spawn failed" end
			return
		end
	end
	SetBodyTransform(offArm.hand, hand)
	SetBodyTransform(offArm.arm, arm)
	offArm.show(true)
	-- Pour la DLL (30 caractères au plus) : les deux corps (de temps en temps), et la caméra
	-- de l'œil pour laquelle ils sont placés, moins la main (recalage au rendu).
	offArm.sendT = offArm.sendT - 1
	if offArm.sendT <= 0 then
		offArm.sendT = 60
		local m = string.format("TDVRW %d %d", offArm.hand, offArm.arm)
		if #m <= 30 then pcall(SetClipboardText, m) end
	end
	local r = VecSub(toWorld(st.eyePose).pos, hand.pos)
	if math.abs(r[1]) < 9 and math.abs(r[2]) < 9 and math.abs(r[3]) < 9 then
		pcall(SetClipboardText, string.format("TDVRE %.3f %.3f %.3f", r[1], r[2], r[3]))
	end
end

-- ---------------------------------------------------------------------------
-- Main libre physique (comme Bonelabs, sur le modèle de l'outil physique plus haut).
-- Un petit corps mobile invisible, un cube de 10 cm (le poing, centré sur la poignée de la
-- manette), est tenu à la manette avec la force du bras qui tient l'outil (celle de la copie
-- physique de la masse, 6 kg : voir FORCE plus bas) : contraintes de vitesse à
-- impulsion limitée posées à chaque pas de physique (tdvrOffHandStep, appelé par update),
-- résolues par le moteur avec les contacts. La main ne traverse ni les murs ni les tables,
-- glisse dessus, pousse et traîne ce qui bouge (un objet lourd résiste). La main et
-- l'avant-bras dessinés (tdvrOffArmTick) sont montrés à la manette + l'écart physique du
-- cube, mesuré au même instant de la simulation (comme l'outil) : contre un obstacle, la
-- main reste à sa surface et l'avant-bras la suit. Vibrations au vrai contact seulement.
-- Collisions : la couche de la copie de l'outil (PHYS_LAYER), avec un masque qui l'exclut.
-- Le joueur et son personnage ignorent cette couche (la main ne le pousse ni ne le coince),
-- et la main ne touche ni la copie de l'outil, ni l'objet qu'elle tient. Elle devient
-- « fantôme » (aucune collision) quand elle est sur le trajet d'un geste du joueur : sur
-- l'axe de la caméra du joueur (rayon de sélection du jeu, tirs partis de l'œil) ou sur
-- celui de la visée de l'autre main (tirs partis de l'outil) pendant un tir, une
-- interaction (sa gâchette) ou une attrape du jeu (grip de l'autre main) ; près de l'objet
-- que porte le jeu ; et quand le rayon de sélection du jeu la choisit. Elle n'arrête donc
-- ni les tirs ni ce rayon ; ailleurs elle reste solide (une main retient une porte pendant
-- que l'autre tire). Elle ne reprend ses collisions qu'une fois dégagée (jamais dans un mur).
-- Attraper (réglage) : grip de cette main avec un objet mobile contre le poing ou à moins
-- de ~8 cm. Le grip de cette main attrape toujours, comme celui de l'autre main (DLL,
-- input.cpp ; l'appui en cours revient dans " ARM ... <grip>" ; s'accroupir est sur la
-- gâchette de cette main). L'objet est alors tenu :
-- il suit la manette avec la force d'un bras (plus il est lourd, plus il est lent ; trop
-- lourd ou bloqué, il glisse de la main) ; grip relâché, il part avec la vitesse de la
-- main (lancer). Tenu, il passe sur la couche de la main (masque inchangé) : le joueur et
-- la main le traversent ; la copie de l'outil et les autres objets lâchés le touchent (on
-- peut les empiler). Ses filtres de collision sont gardés dans une étiquette de chaque forme
-- (« tdvr_lm ») et remis dès qu'il ne touche plus le joueur ni la main (en attendant, la
-- main le traverse aussi).
-- Rangée (corps endormi, fantôme) quand le bras est caché : véhicule, carte, caméra d'un
-- mod loin du joueur, manette perdue, mort. Réglages « Main physique » et « Attraper avec
-- le grip ». Appelé par tdvrOffArmTick (fonctions globales : tick() n'a plus de place
-- pour un upvalue).
-- ---------------------------------------------------------------------------
-- La main seule est aussi forte que la main qui tient la masse : la force, le couple et les
-- vitesses de la copie physique de l'outil (PHYS_FORCE...), et le poids de la masse (6 kg).
-- Elle pousse et frappe un objet lourd comme la masse. Un objet léger n'en part pas plus
-- vite : il prend au plus la vitesse de la main (comme avant, avec 600 N et 1,5 kg).
offHand.FORCE, offHand.TORQUE, offHand.MASS = PHYS_FORCE, PHYS_TORQUE, 6   -- N, N.m, kg : la main seule
-- (un objet tenu, ou tiré au laser, garde la force d'un bras qui porte : 25 kg se soulèvent,
-- lentement ; plus, il serait lancé comme une balle)
offHand.GFORCE, offHand.GTORQUE = 400, 50      -- N, N.m
offHand.MAXV, offHand.MAXW = PHYS_MAXVEL, PHYS_ANGVEL   -- m/s, rad/s
offHand.MAXC = 6                               -- m/s : rattrapage de l'écart au plus (main retenue puis
                                               -- libérée : elle revient sans partir comme une fronde)
offHand.REACH = 0.13                           -- m du centre du poing (~8 cm de sa surface)
offHand.MAXMASS, offHand.MAXSHAPES = 300, 32   -- au-delà : poussé, jamais attrapé
offHand.SLIP = 0.45                            -- m : objet tenu resté si loin de la main 0,4 s : il glisse
offHand.SNAP = 0.8                             -- m : main coincée si loin 0,5 s : elle revient (fantôme)
-- Laser de cette main (voir tdvrOffLaserTick) : attraper de loin, comme le grip de l'autre main
-- (l'attrape du jeu) : l'objet reste tenu à sa distance, au bout du laser.
-- Les règles de l'attrape du jeu (lues dans teardown.exe, voir docs/REVERSE_ENGINEERING.md) :
-- rayon de 3 m ; tout objet mobile de 10 t au plus (portes et véhicules compris) ; tenu au
-- point touché, à sa distance moins 20 cm, 1,5 m de la caméra au moins (ici : FARMIN de la
-- main, ~1,5 m de la tête) ; le point va vers sa place à 0,1 x l'écart par pas de physique
-- (50 m/s au plus), avec une impulsion de clamp(masse, 10, 200) x (1 + écart / 0,8 m, 2 au
-- plus) par pas, et sa rotation suit celle de la main (impulsion 0,5 x la même masse) ; lâché
-- si le point tenu est à plus de 3 m de sa place.
offHand.LRANGE = 3                             -- m : portée
offHand.FARMASS = 10000                        -- kg au plus
offHand.FARSHAPES = 512                        -- formes au plus (pas de limite dans le jeu ; ici : coût de claim)
offHand.FARMIN = 1                             -- m de la main au moins (réglage grabKeepDistance coupé)
-- Réglage grabKeepDistance de la DLL (" ARM ... <keep> <joystick>", coché par défaut) : l'objet reste
-- à la distance où il a été pris (ni 20 cm plus loin, ni 1 m au moins), FARNEAR au moins, presque
-- dans la main (tenu, il traverse le joueur : pas de collision avec lui). stickV : vitesse que le
-- joystick de rotation (haut / bas) donne à cette distance, m/s, + = plus loin (0 : immobile, ou ce
-- joystick est à l'autre main, voir input.cpp GrabStick).
offHand.FARNEAR = 0.15                         -- m de la main au moins
offHand.keep, offHand.stickV = false, 0
offHand.FARDROP = 3                            -- m : point tenu si loin de sa place : lâché
offHand.lt, offHand.panel = 0, false           -- cible du laser (objet à attraper), laser sur un panneau
offHand.far, offHand.farOff, offHand.farEnd = false, nil, nil   -- tenu de loin : écart poignée -> point tenu
offHand.ICON = "ui/hud/crosshair-hand.png"     -- l'icône de main du jeu (son viseur pendant l'attrape)
-- (terre : une matière qui ne brûle pas ; en plastique ou en bois, le poing pourrait prendre feu
-- près d'un incendie et brûler le joueur. Sa masse est ramenée à MASS par SetShapeDensity.)
offHand.XML = '<body tags="tdvr_hand" dynamic="true"><voxbox pos="-0.05 -0.05 -0.05" size="1 1 1" material="dirt"/></body>'

-- Pour la DLL (30 caractères au plus, voir exchange) : un objet est à portée de la main (ou
-- tenu), son grip attrapera ; n : cet objet est sous la main même (pas seulement au bout du
-- laser) - son grip le prend alors au lieu de déplacer le panneau flottant d'un menu de mod.
-- Envoyé quand ça change, et répété tant que c'est vrai (la DLL oublie une valeur de plus
-- d'1,5 s).
offHand.sendReach = function(r, n)
	local o = offHand
	n = (r and n) and true or false
	o.reachN = o.reachN - 1
	if r ~= o.reachSent or n ~= o.nearSent or (r and o.reachN <= 0) then
		o.reachSent, o.nearSent, o.reachN = r, n, 4
		pcall(SetClipboardText, r and (n and "TDVRH 1 1" or "TDVRH 1 0") or "TDVRH 0 0")
	end
end

-- Pour la DLL (drapeau 32 de la requête, voir tick) : cette main tient un objet de loin ; le
-- joystick de rotation (haut / bas) le rapproche ou l'éloigne alors, au lieu de changer d'outil
-- (réglage grabKeepDistance). (Fonction globale : pas une valeur de plus pour tick.)
function tdvrOffFarHold()
	return offHand.held ~= 0 and offHand.far == true
end

-- Rien de solide pour la main (couches 1 à 127) dans le cube du poing (pose t), à 2 cm près ?
-- (point le plus proche du centre, cherché jusqu'à 9 cm : le coin du cube est à 8,7 cm ; puis
-- comparé au cube dans son repère : un coin enfoncé dans un mur compte aussi, et une main
-- posée sur une table redevient solide dès qu'elle s'en écarte de 2 cm)
offHand.clear = function(t)
	QueryRequire("physical")
	if QueryCollisionMask then QueryCollisionMask(255 - PHYS_LAYER) end   -- (la main ignore sa couche)
	rejectPhys()
	local tb = GetToolBody()
	if tb ~= 0 then QueryRejectBody(tb) end
	if offHand.held ~= 0 then QueryRejectBody(offHand.held) end
	local hit, cp = QueryClosestPoint(t.pos, 0.09)
	if not hit then return true end
	local l = TransformToLocalPoint(t, cp)
	return math.abs(l[1]) > 0.07 or math.abs(l[2]) > 0.07 or math.abs(l[3]) > 0.07
end

-- Collisions de la main : fantôme (filtre 0) ou couche PHYS_LAYER, masque sans elle. Rendues
-- seulement une fois la main dégagée.
offHand.setGhost = function(g)
	local o = offHand
	if g == o.ghost or o.shape == 0 or not IsHandleValid(o.shape) then return end
	if not g and not o.clear(GetBodyTransform(o.body)) then return end
	o.ghost = g
	SetShapeCollisionFilter(o.shape, g and 0 or PHYS_LAYER, g and 0 or 255 - PHYS_LAYER)
	if not g then o.quietT = 0.15 end   -- (collisions rendues : pas un choc)
end

-- Le corps de la main (une fois ; refait seulement s'il a disparu).
offHand.spawn = function(pose)
	local o = offHand
	for _, b in ipairs(FindBodies("tdvr_hand", true) or {}) do Delete(b) end   -- (orphelins : partie rechargée)
	local b, s = 0, 0
	for _, h in ipairs(Spawn(o.XML, pose, false, false) or {}) do
		local ty = GetEntityType(h)
		if ty == "body" and b == 0 then
			b = h
		elseif ty == "shape" and s == 0 then
			s = h
		end
	end
	if b == 0 or s == 0 then
		if b ~= 0 then Delete(b) end
		return false
	end
	SetShapeCollisionFilter(s, 0, 0)   -- (fantôme jusqu'à être dégagée : voir setGhost)
	SetTag(s, "invisible")
	SetTag(s, "unbreakable")
	local m0 = GetBodyMass(b)
	if m0 > 0.01 then SetShapeDensity(s, o.MASS / m0) end   -- (multiplie la densité de sa matière)
	o.body, o.shape, o.mass, o.ghost, o.parked = b, s, math.max(GetBodyMass(b), 0.1), true, false
	o.want, o.rp, o.goalEnd, o.stepPending, o.lastRes, o.underN = nil, nil, nil, false, 0, 0
	SetBodyActive(b, true)
	SetBodyVelocity(b, GetPlayerVelocity())
	if camDebug then
		local c = GetBodyCenterOfMass(b)   -- (0 0 0 attendu : le cube est centré sur la poignée)
		physMsg = string.format("offhand: physical hand body %d, mass %.2f kg (before %.2f), centre of mass %.3f %.3f %.3f, dynamic %s",
			b, o.mass, m0, c[1], c[2], c[3], tostring(IsBodyDynamic(b)))
	end
	return true
end

-- Filtres d'origine d'une forme d'un objet tenu (étiquette « tdvr_lm » : « couche_masque »).
offHand.lm = function(s)
	local l, m = string.match(GetTagValue(s, "tdvr_lm") or "", "^(%d+)_(%d+)$")
	return tonumber(l), tonumber(m)
end

-- Deux boîtes (min, max) séparées ?
offHand.apart = function(a0, a1, b0, b1)
	return a1[1] < b0[1] or a0[1] > b1[1] or a1[2] < b0[2] or a0[2] > b1[2] or a1[3] < b0[3] or a0[3] > b1[3]
end

-- Objets lâchés (formes étiquetées « tdvr_lm », sauf l'objet tenu) : filtres d'origine remis
-- dès que l'objet ne touche plus le joueur ni la main (jusque-là ils se traversent : un objet
-- lâché ou lancé ne pousse pas le joueur). force : tous, tout de suite.
offHand.restore = function(force)
	local o = offHand
	local list, fromSet = nil, false
	if fastLua.on and not force then
		-- (fastLua : les formes que la main a prises - offHand.claim - au lieu d'une recherche de
		-- l'étiquette dans toute la scène à chaque vérification. Celle-ci reste, ajoutée à la liste,
		-- pour les morceaux cassés d'un objet tenu ou lâché (ils portent l'étiquette sans être dans
		-- la liste) : à la première vérification après chaque lâcher, puis une sur quatre (chaque
		-- seconde), et à chaque fois quand la liste n'a rien d'autre que l'objet tenu - comme avant)
		o.lmSet = o.lmSet or {}
		list, fromSet = {}, true
		local other = false
		for s in pairs(o.lmSet) do
			if IsHandleValid(s) and HasTag(s, "tdvr_lm") then
				list[#list + 1] = s
				if not other and (o.held == 0 or GetShapeBody(s) ~= o.held) then other = true end
			else
				o.lmSet[s] = nil
			end
		end
		o.restoreScan = (o.restoreScan or 0) - 1
		if not other or o.restoreScan <= 0 then
			o.restoreScan = 4
			fromSet = false
			for _, s in ipairs(FindShapes("tdvr_lm", true) or {}) do
				if not o.lmSet[s] then
					o.lmSet[s] = true
					list[#list + 1] = s
				end
			end
		end
	else
		list = FindShapes("tdvr_lm", true) or {}
		if force then o.lmSet = {} end
	end
	if #list == 0 then
		o.toRestore = false
		return
	end
	local pp = GetPlayerTransform().pos
	local pmn, pmx = VecAdd(pp, Vec(-0.55, -0.2, -0.55)), VecAdd(pp, Vec(0.55, 2.0, 0.55))   -- (le joueur)
	local hmn, hmx
	if o.body ~= 0 and IsHandleValid(o.body) then
		hmn, hmx = GetBodyBounds(o.body)
		hmn, hmx = VecSub(hmn, Vec(0.02, 0.02, 0.02)), VecAdd(hmx, Vec(0.02, 0.02, 0.02))
	end
	local free, n = {}, 0
	for _, s in ipairs(list) do
		local b = GetShapeBody(s)
		if force or o.held == 0 or b ~= o.held then
			if free[b] == nil then
				local f = force or b == 0 or not IsHandleValid(b)
				if not f then
					local mn, mx = GetBodyBounds(b)
					f = o.apart(mn, mx, pmn, pmx) and (not hmn or o.apart(mn, mx, hmn, hmx))
				end
				free[b] = f
			end
			if free[b] then
				local l, m = o.lm(s)
				if l and m then SetShapeCollisionFilter(s, l, m) end
				RemoveTag(s, "tdvr_lm")
				if o.lmSet then o.lmSet[s] = nil end
				n = n + 1
			end
		end
	end
	if n > 0 and camDebug and not debugMsg then
		debugMsg = string.format("offhand: collision filters restored on %d shapes (%d still waiting)", n, #list - n)
	end
	-- (liste de la main : la recherche dans la scène vérifie encore une fois qu'il ne reste rien)
	o.toRestore = n < #list or fromSet
end

-- À chaque image normale (tdvrOffArmTick) : vérification environ 4 fois par seconde.
offHand.restoreTick = function()
	local o = offHand
	if not o.toRestore then return end
	o.restoreN = (o.restoreN or 0) - 1
	if o.restoreN > 0 then return end
	o.restoreN = 15
	o.restore(false)
end

-- Objet à attraper près du point p (le centre du poing) : le plus proche à moins de REACH,
-- mobile, ni trop lourd ni fait de trop de formes, ni un véhicule, ni ce que porte le jeu,
-- ni ce sur quoi le joueur est debout. Renvoie le corps (0 = rien), le point le plus
-- proche et la normale de la surface en ce point.
offHand.find = function(p)
	local o = offHand
	local tb, gb = GetToolBody(), GetPlayerGrabBody()
	local gc, gs = GetPlayerGroundContact()
	local under = (gc and gs and gs ~= 0) and GetShapeBody(gs) or 0
	o.rej = o.rej or {}
	for attempt = 1, 3 do
		QueryRequire("physical dynamic")
		if tb ~= 0 then QueryRejectBody(tb) end
		if gb ~= 0 then QueryRejectBody(gb) end
		rejectPhys()
		for i = 1, attempt - 1 do QueryRejectBody(o.rej[i]) end
		local hit, cp, cn, s = QueryClosestPoint(p, o.REACH)
		if not hit or s == 0 then return 0 end
		local b = GetShapeBody(s)
		if b == 0 then return 0 end
		if b ~= under and IsBodyDynamic(b) and GetBodyVehicle(b) == 0 and GetBodyMass(b) <= o.MAXMASS
			and #(GetBodyShapes(b) or {}) <= o.MAXSHAPES then
			return b, cp, cn
		end
		o.rej[attempt] = b
	end
	return 0
end

-- L'objet b passe sur la couche de la main, masque inchangé (filtres d'origine gardés dans
-- l'étiquette « tdvr_lm » de chaque forme) : le joueur et la main le traversent. Renvoie ses formes.
offHand.claim = function(b)
	local shapes = GetBodyShapes(b) or {}
	offHand.lmSet = offHand.lmSet or {}   -- (formes prises, pour offHand.restore : fastLua)
	for _, s in ipairs(shapes) do
		if not HasTag(s, "tdvr_lm") then
			local l, m = GetShapeCollisionFilter(s)
			SetTag(s, "tdvr_lm", string.format("%d_%d", l or 1, m or 255))
		end
		offHand.lmSet[s] = true
		local l, m = offHand.lm(s)
		if l ~= 0 then SetShapeCollisionFilter(s, PHYS_LAYER, m or 255) end   -- (une forme sans collision le reste)
	end
	return shapes
end

-- Prend l'objet b : la main se pose sur sa surface (paume à 4 cm du point le plus proche cp,
-- normale cn), dans son orientation actuelle (pose hand) ; l'objet passe sur la couche de la
-- main (claim).
offHand.grab = function(b, cp, cn, hand)
	local o = offHand
	local d = VecSub(hand.pos, cp)
	local n = cn
	if VecDot(n, d) < 0 then n = VecScale(n, -1) end
	local gp = hand.pos
	if VecLength(d) > 0.005 and VecLength(n) > 0.5 then gp = VecAdd(cp, VecScale(VecNormalize(n), 0.04)) end
	o.hold = TransformToLocalTransform(GetBodyTransform(b), Transform(gp, hand.rot))
	local shapes = o.claim(b)
	o.held, o.heldMass, o.jointed = b, math.max(GetBodyMass(b), 0.1), IsBodyJointedToStatic(b)
	o.far, o.farOff, o.farEnd = false, nil, nil
	-- (goalEnd gardé : la main reste montrée où elle est jusqu'au premier pas qui tire l'objet)
	o.slipT, o.stepPending, o.lastRes, o.quietT = 0, false, 0, 0.2
	o.hapT, o.hapA = 0.06, 0.35   -- (la main se referme sur l'objet : un vrai contact)
	o.pressT = 1   -- (cet appui du grip a servi : glissé ou cassé, l'objet n'est pas repris sans nouvel appui)
	if camDebug then
		debugMsg = string.format("offhand: grab body %d, mass %.1f kg, %d shapes, jointed %s, hand %.3f m from it",
			b, o.heldMass, #shapes, tostring(o.jointed), VecLength(d))
	end
end

-- Prend de loin l'objet b au point p (touché par le laser, pose aim, échelle du monde s), comme
-- l'attrape du jeu : il reste au bout du laser, à sa distance (réglage grabKeepDistance : FARNEAR
-- au moins ; coupé, comme le jeu : moins 20 cm, FARMIN au moins, atteints en quelques images,
-- voir farSlide) ; le point p suit la poignée de la manette (pose
-- hand) comme s'il était au bout d'un bâton tenu par la main (il tourne avec elle). L'objet
-- passe sur la couche de la main (claim) : il traverse le joueur.
offHand.grabFar = function(b, p, hand, aim, s)
	local o = offHand
	o.hold = TransformToLocalTransform(GetBodyTransform(b), Transform(p, hand.rot))
	local shapes = o.claim(b)
	o.held, o.heldMass, o.jointed = b, math.max(GetBodyMass(b), 0.1), IsBodyJointedToStatic(b)
	o.far, o.farOff, o.farEnd = true, TransformToLocalPoint(hand, p), nil
	-- (distance le long du laser : farOff glisse sur l'axe du laser, dans le repère de la poignée)
	local dir = QuatRotateVec(aim.rot, Vec(0, 0, -1))
	o.farOff0, o.farDir = o.farOff, TransformToLocalVec(hand, dir)
	o.farD0 = math.max(0, VecDot(VecSub(p, aim.pos), dir))
	o.farLen, o.stickMoved = o.farD0, false
	o.farGoal = o.keep and math.max(o.FARNEAR * s, o.farD0) or math.max(o.FARMIN * s, o.farD0 - 0.2 * s)
	o.slipT, o.stepPending, o.lastRes, o.quietT = 0, false, 0, 0.2
	o.hapT, o.hapA = 0.05, 0.3
	o.pressT = 1   -- (cet appui du grip a servi : glissé, cassé ou perdu, l'objet n'est pas repris sans nouvel appui)
	SetBodyActive(b, true)
	if camDebug then
		debugMsg = string.format("offhand: far grab of body %d, mass %.1f kg, %d shapes, jointed %s, %.2f m from the hand, taken at %.2f m along the laser, held at %.2f m (keep %s)",
			b, o.heldMass, #shapes, tostring(o.jointed), VecLength(o.farOff), o.farD0, o.farGoal, tostring(o.keep))
	end
end

-- Rotation par crans ou recentrage pendant une prise de loin (tdvrOffHandTick) : le « bâton »
-- saute avec la manette (au bout de 3 m, 30° = 1,7 m) ; l'objet saute avec lui, dans la même
-- pose par rapport à la poignée (old : sa pose visée à l'image précédente, pp : le joueur),
-- au lieu de traverser la pièce en retard et d'être lâché. Jamais à travers un mur (rien de
-- solide entre la main et le point tenu), ni un objet lié (porte, chaîne) : il suit alors par
-- la physique, comme avant.
offHand.carry = function(old, ik, pp)
	local o = offHand
	local h = o.held
	if h == 0 or not o.far or not o.hold or o.jointed or not IsHandleValid(h) then return end
	if GetJointedBodies and #(GetJointedBodies(h) or {}) > 1 then return end
	local moved = o.ptick and VecSub(pp, o.ptick) or Vec(0, 0, 0)
	if VecLength(moved) > 1 then moved = Vec(0, 0, 0) end
	local rel = TransformToLocalTransform(Transform(VecAdd(old.pos, moved), old.rot), GetBodyTransform(h))
	local nt = TransformToParentTransform(ik, rel)
	local d = VecSub(TransformToParentPoint(nt, o.hold.pos), ik.pos)
	local l = VecLength(d)
	if l > 0.05 then
		QueryRequire("physical")
		QueryRejectBody(h)
		local tb = GetToolBody()
		if tb ~= 0 then QueryRejectBody(tb) end
		rejectPhys()
		if QueryRejectPlayer then pcall(QueryRejectPlayer) end
		if QueryRaycast(ik.pos, VecScale(d, 1 / l), l) then return end
	end
	SetBodyTransform(h, nt)
	SetBodyVelocity(h, GetPlayerVelocity())
	SetBodyAngularVelocity(h, Vec(0, 0, 0))
	o.farEnd, o.slipT = nil, 0
	if camDebug then debugMsg = string.format("offhand: far-held body %d carried with the snap turn / recenter", h) end
end

-- Lâche l'objet tenu : il garde sa vitesse (lancé à la vitesse de la main ; 15 m/s au plus).
-- Ses filtres seront remis par restore. throw : lâché par le grip ; why : sinon, pourquoi
-- (journal).
offHand.drop = function(throw, why)
	local o = offHand
	local b = o.held
	if b == 0 then return end
	local far = o.far
	o.held, o.stepPending = 0, false   -- (goalEnd gardé : pas de saut de la main montrée)
	o.far, o.farOff, o.farEnd = false, nil, nil
	o.toRestore, o.restoreN = true, 5
	o.restoreScan = 0   -- (fastLua : la première vérification cherche aussi dans la scène, voir restore)
	o.ghostT = math.max(o.ghostT or 0, 0.2)   -- (la main, fantôme, s'écarte de l'objet)
	if IsHandleValid(b) then
		local v = GetBodyVelocity(b)
		local sp = VecLength(v)
		if sp > 15 then SetBodyVelocity(b, VecScale(v, 15 / sp)) end
		if camDebug then
			debugMsg = string.format("offhand: release body %d (%s%s), speed %.2f m/s", b,
				throw and "grip released" or ("dropped: " .. tostring(why or "?")), far and ", held at a distance" or "", sp)
		end
	end
end

-- Bras caché : la main lâche ce qu'elle tient, devient fantôme et s'endort sur place.
offHand.park = function()
	local o = offHand
	o.drop(false, "arm hidden: " .. tostring(o.hideWhy or "?"))
	if o.body ~= 0 and IsHandleValid(o.body) and not o.parked then
		o.parked = true
		o.setGhost(true)
		SetBodyVelocity(o.body, Vec(0, 0, 0))
		SetBodyAngularVelocity(o.body, Vec(0, 0, 0))
		SetBodyActive(o.body, false)
	end
	o.want, o.rp, o.goalEnd, o.stepPending = nil, nil, nil, false
	o.underN = 0   -- (fastLua : bras de nouveau visible, l'objet sous la main est cherché tout de suite)
	o.sendReach(false)
end

-- Réglage coupé, VR arrêtée : main supprimée.
offHand.destroy = function()
	local o = offHand
	o.drop(false, "physical hand removed")
	if o.body ~= 0 and IsHandleValid(o.body) then Delete(o.body) end
	o.body, o.shape, o.ghost, o.parked = 0, 0, nil, false
	o.want, o.rp, o.goalEnd, o.stepPending = nil, nil, nil, false
	o.sendReach(false)
end

-- Résultat du dernier pas de physique (comme physResidual) : la vitesse obtenue par le corps
-- tiré (la main, ou l'objet qu'elle tient, au point tenu) comparée à celle que le bras
-- pouvait lui donner. Sans contact l'écart est presque nul ; un obstacle le crée.
offHand.residual = function()
	local o = offHand
	if not o.stepPending then return end
	o.stepPending = false
	local b = o.stepBody
	if not b or b == 0 or not IsHandleValid(b) or not o.cmdV or not o.preV then return end
	local v = GetBodyVelocityAtPos(b, TransformToParentPoint(GetBodyTransform(b), o.stepLocal))
	local lim = o.stepForce / 60 / math.max(o.stepMass or 1, 0.1)   -- vitesse que le bras donne en un pas
	local e = Vec(0, 0, 0)
	for i = 1, 3 do e[i] = v[i] - (o.preV[i] + clamp(o.cmdV[i] - o.preV[i], -lim, lim)) end
	local r = VecLength(e)
	o.resMax = math.max(o.resMax or 0, r)
	o.resRise = math.max(o.resRise or 0, r - (o.lastRes or 0))
	o.lastRes = r
end

-- Un autre objet solide qui peut toucher la main ou l'objet tenu (couches 1 à 127, masque
-- avec la couche de la main, comme physNearOther : pas le personnage du joueur) est-il à
-- moins de 3 cm du corps b ?
offHand.near = function(b)
	local mn, mx = GetBodyBounds(b)
	local m = Vec(0.03, 0.03, 0.03)
	local tb = GetToolBody()
	for _, s in ipairs(QueryAabbShapes(VecSub(mn, m), VecAdd(mx, m)) or {}) do
		local sb = GetShapeBody(s)
		if sb ~= b and sb ~= offHand.body and sb ~= tb and sb ~= phys.body then
			local l, mk = GetShapeCollisionFilter(s)
			if l and l % PHYS_LAYER ~= 0 and math.floor((mk or 0) / PHYS_LAYER) % 2 == 1 then return true end
		end
	end
	return false
end

-- ik : poignée de la manette (repère ik de la main, monde, image de l'œil gauche) ; nil =
-- bras caché, la main est rangée. grab : réglage « attraper avec le grip ». aim : visée de
-- l'autre main (monde ; ses tirs partent sur cet axe). Renvoie la poignée à montrer (la
-- manette + l'écart physique), ou nil.
function tdvrOffHandTick(st, ik, grab, aim)
	local o = offHand
	if not ik then
		o.park()
		return nil
	end
	local dt = lastTickDt
	if o.body ~= 0 and not (IsHandleValid(o.body) and IsHandleValid(o.shape)) then
		-- (corps ou forme supprimés par le niveau ou un autre mod : refaite un peu plus tard,
		-- pas à chaque image)
		o.drop(false, "physical hand body lost")
		if IsHandleValid(o.body) then Delete(o.body) end
		o.body, o.shape, o.ghost, o.want, o.goalEnd, o.stepPending = 0, 0, nil, nil, nil, false
		o.waitT = 30
		if camDebug and not debugMsg then debugMsg = "offhand: physical hand body lost, made again soon" end
	end
	if o.body == 0 then
		-- (après un échec, nouvel essai 10 s plus tard : pas un Spawn à chaque image)
		if o.waitT > 0 then
			o.waitT = o.waitT - 1
			o.sendReach(false)
			return ik
		end
		if not o.spawn(ik) then
			o.waitT = 600
			o.sendReach(false)
			if camDebug and not debugMsg then debugMsg = "offhand: spawn failed" end
			return ik
		end
	end
	local b = o.body
	if o.parked then
		-- (bras de nouveau visible : la main revient à la manette, fantôme jusqu'à être dégagée)
		o.parked = false
		SetBodyActive(b, true)
		SetBodyTransform(b, ik)
		SetBodyVelocity(b, GetPlayerVelocity())
	end
	-- Le joueur et son personnage ignorent la couche de la main (comme celle de la copie).
	physSetPlayerMask(true)
	physPlayerFilter(dt)
	-- Le bouton « attraper » du jeu ne doit jamais saisir la main elle-même.
	if GetPlayerGrabBody() == b then
		ReleasePlayerGrab()
		if camDebug then debugMsg = "offhand: the game's grab took the physical hand - released" end
	end
	-- Mouvement de la main PAR RAPPORT AU JOUEUR, lissé ; le déplacement du joueur est suivi
	-- pas à pas dans tdvrOffHandStep (comme l'outil physique, voir physTool).
	local pp = GetPlayerTransform().pos
	local rp = VecSub(ik.pos, pp)
	local jump = o.yaw ~= nil and ((st.turnMode == 0 and vrYaw ~= o.yaw) or st.recenter ~= o.recenter)
	o.yaw, o.recenter = vrYaw, st.recenter
	-- (rotation par crans, recentrage : l'objet tenu de loin saute avec la main, voir carry)
	if jump and o.far and o.held ~= 0 and o.want then o.carry(o.want, ik, pp) end
	if o.rp and o.want and dt > 0.003 then
		local hv = VecScale(VecSub(rp, o.rp), 1 / dt)
		local av = VecScale(quatToRotVec(QuatRotateQuat(ik.rot, quatConj(o.want.rot))), 1 / dt)
		if jump or VecLength(hv) > 12 then
			jump = true   -- (rotation par crans, recentrage : pas de vitesse héritée du saut)
			o.hrv, o.tav = Vec(0, 0, 0), Vec(0, 0, 0)
		else
			o.hrv = VecLerp(o.hrv or hv, hv, 0.5)
			if VecLength(av) > 30 then av = Vec(0, 0, 0) end
			o.tav = VecLerp(o.tav or av, av, 0.5)
		end
	elseif not o.rp then
		o.hrv, o.tav = Vec(0, 0, 0), Vec(0, 0, 0)
	end
	o.rp, o.ptick, o.want, o.steps = rp, pp, ik, 0
	-- Attraper : grip de cette main, appui décidé par la DLL (2 = attraper, voir input.cpp).
	local press = grab and o.press or 0
	if o.held ~= 0 then
		local h = o.held
		if press ~= 2 then
			o.drop(true)   -- grip relâché : l'objet part avec la vitesse de la main
		elseif not IsHandleValid(h) or not IsBodyDynamic(h) or #(GetBodyShapes(h) or {}) == 0 then
			o.drop(false, "broken or became static")
		elseif GetPlayerGrabBody() == h then
			o.drop(false, "taken by the game's grab (other hand)")
		end
	elseif press == 2 then
		-- (0,3 s de tolérance : l'objet peut arriver sous la main, ou sous le laser, juste après
		-- l'appui) Sous la main : pris dans la main ; sinon, au bout du laser : tenu de loin.
		if o.prevPress ~= 2 then o.pressT = 0 end
		o.pressT = (o.pressT or 0) + dt
		if o.pressT < 0.3 then
			local hp = GetBodyTransform(b)
			local hb, cp, cn = o.find(hp.pos)
			if hb ~= 0 then
				o.grab(hb, cp, cn, hp)
			elseif o.lt ~= 0 and IsHandleValid(o.lt) and o.ltPoint and o.ltAim then
				o.grabFar(o.lt, o.ltPoint, ik, o.ltAim, o.ltS or 1)
			end
		end
	end
	o.prevPress = press
	local holding = o.held ~= 0
	local bt = GetBodyTransform(b)
	if holding then
		-- Objet trop lourd ou bloqué : resté trop loin de la main (tenu de loin : du bout du
		-- laser), il glisse de la main.
		local ht = GetBodyTransform(o.held)
		local want = o.goalEnd and o.goalEnd.pos
		if o.far then want = o.farEnd end   -- (nil avant le premier pas : pas encore d'écart)
		local gap = want and VecLength(VecSub(TransformToParentPoint(ht, o.hold.pos), want)) or 0
		if gap > o.SLIP and not o.far then o.slipT = (o.slipT or 0) + dt else o.slipT = 0 end
		if o.slipT > 0.4 or gap > (o.far and o.FARDROP * st.worldScale or 2) then
			o.drop(false, string.format("too far from its place, %.2f m", gap))
			holding = false
		elseif o.far then
			-- Tenu de loin : la distance va vers sa cible (comme le jeu : la moitié de l'écart par
			-- image, 0,5 m au plus) ; la main (fantôme) reste à la manette.
			-- (réglage grabKeepDistance : le joystick de rotation, haut / bas, l'éloigne ou le rapproche,
			-- de FARNEAR jusqu'à la portée du laser ; vitesse donnée par la DLL)
			if o.keep and o.stickV ~= 0 then
				local ws = st.worldScale
				o.farGoal = clamp(o.farGoal + o.stickV * ws * dt, o.FARNEAR * ws, o.LRANGE * ws)
				o.stickMoved = true
			elseif o.stickMoved then
				o.stickMoved = false
				if camDebug then debugMsg = string.format("offhand: far-held body moved with the stick, held at %.2f m along the laser", o.farGoal) end
			end
			o.farLen = o.farLen + clamp(0.5 * (o.farGoal - o.farLen), -0.5, 0.5)
			o.farOff = VecAdd(o.farOff0, VecScale(o.farDir, o.farLen - o.farD0))
			SetBodyTransform(b, ik)
			SetBodyVelocity(b, VecAdd(GetPlayerVelocity(), o.hrv or Vec(0, 0, 0)))
			SetBodyAngularVelocity(b, Vec(0, 0, 0))
			bt = ik
		else
			-- La main (fantôme) reste sur l'objet : prête quand il sera lâché.
			local hpose = TransformToParentTransform(ht, o.hold)
			SetBodyTransform(b, hpose)
			SetBodyVelocity(b, GetBodyVelocityAtPos(o.held, hpose.pos))
			bt = hpose
		end
	end
	if not holding then
		-- Coincée loin de la manette (ou rotation par crans, recentrage) : elle y revient,
		-- fantôme jusqu'à être dégagée.
		local gap = VecLength(VecSub(bt.pos, ik.pos))
		if gap > o.SNAP then o.stuckT = (o.stuckT or 0) + dt else o.stuckT = 0 end
		if jump or gap > 2.5 or o.stuckT > 0.5 or not validRot(bt.rot) then
			o.setGhost(true)
			SetBodyTransform(b, ik)
			SetBodyVelocity(b, GetPlayerVelocity())
			SetBodyAngularVelocity(b, Vec(0, 0, 0))
			o.stuckT, o.goalEnd, o.stepPending, o.lastRes = 0, ik, false, 0
			bt = ik
		end
	end
	-- Fantôme : objet tenu ; main sur le trajet d'un geste du joueur pendant ce geste (tir :
	-- gâchette de l'outil ; interaction : gâchette de cette main ; attrape du jeu : grip de
	-- l'autre main), c'est-à-dire sur l'axe de la caméra du joueur (rayon de sélection du jeu,
	-- vers la cible du laser ; tirs partis de l'œil) ou sur celui de la visée de l'autre main
	-- (tirs partis de l'outil, dispersion comprise) ; près de l'objet que porte le jeu ; et
	-- main choisie par le rayon de sélection du jeu. Ailleurs elle reste solide pendant un tir.
	local lh = st.leftHanded == 1
	local domTrig, domGrip = lh and st.ltrig or st.rtrig, lh and st.lsq or st.rsq
	local pc = GetPlayerCameraTransform()
	local f = QuatRotateVec(pc.rot, Vec(0, 0, -1))
	local along = VecDot(VecSub(bt.pos, pc.pos), f)
	local onLine = along > 0 and along < 4 and axisDist(bt.pos, pc.pos, f) < 0.12   -- (coin du cube : 8,7 cm)
	local onAim = false
	if aim then
		local af = QuatRotateVec(aim.rot, Vec(0, 0, -1))
		local aa = VecDot(VecSub(bt.pos, aim.pos), af)
		onAim = aa > -0.3 and aa < 4 and axisDist(bt.pos, aim.pos, af) < 0.25
	end
	o.picked = onLine and (o.picked or GetPlayerPickBody() == b)
	local gb = GetPlayerGrabBody()
	local nearGrab = false
	if gb ~= 0 and IsHandleValid(gb) then
		local mn, mx = GetBodyBounds(gb)
		local m = Vec(0.25, 0.25, 0.25)
		nearGrab = not o.apart(VecSub(mn, m), VecAdd(mx, m), bt.pos, bt.pos)
	end
	local gesture = domTrig > 0.25 or InputDown("interact") or domGrip > 0.25 or InputDown("usetool")
	-- (coup de masse du jeu en cours, voir updateTool : sa sphère de 0,3 m part de l'œil ; la
	-- main est fantôme si elle est à moins de 0,4 m de son axe, jusqu'à 3,5 m)
	if (gesture and (onLine or onAim)) or nearGrab or o.picked
		or ((phys.strikeT or 0) > 0 and along > 0 and along < 3.5 and axisDist(bt.pos, pc.pos, f) < 0.4) then
		o.ghostT = 0.3
	else
		o.ghostT = math.max(0, (o.ghostT or 0) - dt)
	end
	o.setGhost(holding or o.ghostT > 0)
	-- Vibrations au vrai contact seulement (comme l'outil physique) : un choc (le corps tiré
	-- freiné net) et, main libre retenue par un obstacle, une légère vibration d'appui ; un
	-- autre objet doit bien être à moins de 3 cm.
	o.residual()   -- (dernier pas de physique de l'image précédente)
	local res, rise = o.resMax or 0, o.resRise or 0
	o.resMax, o.resRise = 0, 0
	o.quietT = math.max(0, (o.quietT or 0) - dt)
	local hap = 0
	local hitV = holding and 1.2 or PHYS_HIT
	if o.quietT == 0 and (holding or not o.ghost) and (rise > hitV or (not holding and res > PHYS_PRESS))
		and o.near(holding and o.held or b) then
		if rise > hitV then
			o.hapA = math.max((o.hapT or 0) > 0 and (o.hapA or 0) or 0, math.min(1, 0.2 + rise / 5))
			o.hapT = 0.07
		end
		if not holding and res > PHYS_PRESS then hap = math.min(0.25, 0.06 + (res - PHYS_PRESS) * 0.04) end
	end
	if (o.hapT or 0) > 0 then
		hap = math.max(hap, o.hapA or 0)
		o.hapT = o.hapT - dt
	end
	if hap > 0 then
		if lh then hapR = math.max(hapR, hap) else hapL = math.max(hapL, hap) end
	end
	-- Pour la DLL (journal) : un objet à portée, au bout du laser (o.lt, voir tdvrOffLaserTick)
	-- ou tenu ; et à part, un objet sous la main même (son grip le prend : il ne déplace pas le
	-- panneau d'un menu de mod). (pas cherché pendant un appui du grip)
	local under
	if fastLua.on then
		-- (fastLua : cherché une image normale sur trois, et aussitôt quand la recherche redevient
		-- utile ; à chaque image dès que le grip de cette main se ferme (plus de 0,3) : la DLL lit
		-- cette valeur quand il passe 0,6, pour donner l'appui à l'objet plutôt qu'au panneau d'un mod)
		local want = grab and not holding and o.press == 0
		if not want then
			o.underN = 0
		else
			o.underN = (o.underN or 0) - 1
			local offSq = (st.leftHanded == 1 and st.rsq or st.lsq) or 0
			if o.underN <= 0 or offSq > 0.3 then
				o.underN = 3
				o.under = o.find(bt.pos) ~= 0
			end
		end
		under = want and o.under
	else
		under = grab and not holding and o.press == 0 and o.find(bt.pos) ~= 0
	end
	o.sendReach(grab and (holding or (o.press == 0 and (o.lt ~= 0 or under))), under)
	-- Écart physique : pose obtenue à la fin du dernier pas comparée à la pose voulue au même
	-- instant (tdvrOffHandStep) ; la main est montrée à la manette + cet écart (0,9 m au plus).
	local shown = ik
	if o.goalEnd and not (holding and o.far) then   -- (tenu de loin : la main reste à la manette)
		local got = holding and TransformToParentTransform(GetBodyTransform(o.held), o.hold) or GetBodyTransform(b)
		local dev = VecSub(got.pos, o.goalEnd.pos)
		local dl = VecLength(dev)
		if dl > 0.9 then dev = VecScale(dev, 0.9 / dl) end
		shown = Transform(VecAdd(ik.pos, dev), QuatRotateQuat(QuatRotateQuat(got.rot, quatConj(o.goalEnd.rot)), ik.rot))
	end
	if camDebug then
		o.logT = (o.logT or 0) + dt
		o.maxDev = math.max(o.maxDev or 0, VecLength(VecSub(shown.pos, ik.pos)))
		o.maxRes, o.maxRise, o.maxHap = math.max(o.maxRes or 0, res), math.max(o.maxRise or 0, rise), math.max(o.maxHap or 0, hap)
		-- (le joueur debout sur la main ? jamais : il ignore sa couche)
		local gc, gs = GetPlayerGroundContact()
		if gc and gs and gs ~= 0 and GetShapeBody(gs) == b then o.onHand = (o.onHand or 0) + 1 end
		if o.logT > 1 and not debugMsg then
			o.logT = 0
			local l, m = GetShapeCollisionFilter(o.shape)
			local fg = 0
			if holding and o.far and o.farEnd then
				fg = VecLength(VecSub(TransformToParentPoint(GetBodyTransform(o.held), o.hold.pos), o.farEnd))
			end
			debugMsg = string.format("offhand: shown dev max %.3f m, residual max %.2f rise %.2f, hap max %.2f, ghost %s (layer %s mask %s), grip %d, reach %s, held %d (%.1f kg), restore pending %s, player on hand %d, hand mass %.2f, player mask %s, laser target %d, far %s (%.2f m, lag %.2f m)",
				o.maxDev, o.maxRes, o.maxRise, o.maxHap, tostring(o.ghost), tostring(l), tostring(m), o.press,
				tostring(o.reachSent), o.held, holding and o.heldMass or 0, tostring(o.toRestore), o.onHand or 0,
				GetBodyMass(b), tostring(GetPlayerParam("CollisionMask")), o.lt, tostring(holding and o.far),
				(holding and o.far and o.farOff) and VecLength(o.farOff) or 0, fg)
			o.maxDev, o.maxRes, o.maxRise, o.maxHap, o.onHand = 0, 0, 0, 0, 0
		end
	end
	return shown
end

-- Pas de physique (appelé par update) : la main, ou l'objet qu'elle tient (au point tenu),
-- est tirée vers la poignée de la manette avec une force et un couple limités, comme la copie
-- de l'outil : chaque pas vise la main là où elle sera à ce moment (déplacement réel du
-- joueur compris, mouvement de la main prolongé). Un objet lié au décor (porte) n'est tiré
-- qu'en position : il tourne sur ses gonds.
function tdvrOffHandStep(dt)
	local o = offHand
	if not o.want or dt <= 0 or o.parked then return end
	local holding = o.held ~= 0
	local b = holding and o.held or o.body
	if b == 0 or not IsHandleValid(b) then return end
	SetBodyActive(b, true)
	o.residual()   -- (résultat du pas précédent : contact ?)
	local bt = GetBodyTransform(b)
	local loc = holding and o.hold.pos or Vec(0, 0, 0)
	local p = TransformToParentPoint(bt, loc)
	local k = o.steps or 0
	o.steps = k + 1
	local pp = GetPlayerTransform().pos
	local moved = o.ptick and VecSub(pp, o.ptick) or Vec(0, 0, 0)
	if VecLength(moved) > 1 then moved = Vec(0, 0, 0) end   -- (téléportation : le tick suivant replace la main)
	local hrv, tav = o.hrv or Vec(0, 0, 0), o.tav or Vec(0, 0, 0)
	local rot = o.want.rot
	local rv = VecScale(tav, dt * k)
	local a = VecLength(rv)
	if a > 1e-5 then rot = QuatRotateQuat(QuatAxisAngle(VecScale(rv, 1 / a), math.deg(a)), rot) end
	local goal = VecAdd(VecAdd(o.want.pos, moved), VecScale(hrv, dt * k))
	local pv = GetPlayerVelocity()
	-- Tenu de loin : le point tenu va au bout du « bâton » (farOff dans le repère de la
	-- poignée), qui tourne avec la main : sa vitesse compte aussi la rotation de la main.
	local tgt, ff = goal, VecAdd(pv, hrv)
	local far = holding and o.far and o.farOff
	if far then
		local off = QuatRotateVec(rot, o.farOff)
		tgt = VecAdd(goal, off)
		ff = VecAdd(ff, VecCross(tav, off))
	end
	-- (rattrapage de l'écart borné : une main retenue par un obstacle puis libérée revient
	-- vite, sans partir comme une fronde ; le mouvement de la main, lui, n'est pas borné ici)
	-- Tenu de loin, comme l'attrape du jeu : 0,1 x l'écart par pas (6/s à 60 Hz), 50 m/s au
	-- plus, impulsion clamp(masse, 10, 200) x (1 + écart / 0,8 m, 2 au plus) par pas de 1/60 s
	-- (12 000 à 24 000 N pour 200 kg et plus : 10 t ne se soulèvent pas, elles se traînent).
	local err = VecSub(tgt, p)
	local corr = VecScale(err, (far and 0.1 or PHYS_GAIN) / dt)
	local cl = VecLength(corr)
	local cmax = far and 50 or o.MAXC
	if cl > cmax then corr = VecScale(corr, cmax / cl) end
	local v = VecAdd(ff, corr)
	local sp = VecLength(v)
	if sp > o.MAXV and not far then v = VecScale(v, o.MAXV / sp) end
	local mc = clamp(o.heldMass or 1, 10, 200)
	local force = holding and o.GFORCE or o.FORCE
	if far then force = mc * (1 + clamp(VecLength(err) / 0.8, 0, 1)) * 60 end
	local imp = force * dt
	for i = 1, 3 do ConstrainVelocity(b, 0, p, AXES[i], v[i], -imp, imp) end
	if not (holding and o.jointed) then
		local goalRot = holding and QuatRotateQuat(rot, quatConj(o.hold.rot)) or rot
		local w = VecAdd(tav, VecScale(quatToRotVec(QuatRotateQuat(goalRot, quatConj(bt.rot))), PHYS_GAIN / dt))
		local wl = VecLength(w)
		if wl > o.MAXW then w = VecScale(w, o.MAXW / wl) end
		local aimp = (far and 0.5 * mc * 60 or (holding and o.GTORQUE or o.TORQUE)) * dt
		for i = 1, 3 do ConstrainAngularVelocity(b, 0, AXES[i], w[i], -aimp, aimp) end
	end
	-- Pour la suite : pose voulue de la main à la fin de ce pas (affichage), consigne et
	-- vitesse de départ (contact, residual).
	local re = VecScale(tav, dt)
	local ae = VecLength(re)
	local rotEnd = rot
	if ae > 1e-5 then rotEnd = QuatRotateQuat(QuatAxisAngle(VecScale(re, 1 / ae), math.deg(ae)), rot) end
	o.goalEnd = Transform(VecAdd(goal, VecScale(VecAdd(pv, hrv), dt)), rotEnd)
	if far then o.farEnd = VecAdd(o.goalEnd.pos, QuatRotateVec(rotEnd, o.farOff)) end
	o.cmdV, o.preV, o.stepPending = v, GetBodyVelocityAtPos(b, p), true
	o.stepBody, o.stepLocal, o.stepForce, o.stepMass = b, loc, force, holding and o.heldMass or o.mass
end

-- Partie (re)chargée : corps de main orphelins supprimés, filtres des objets tenus remis.
function tdvrOffHandCleanup()
	for _, b in ipairs(FindBodies("tdvr_hand", true) or {}) do Delete(b) end
	offHand.body, offHand.shape, offHand.held = 0, 0, 0
	offHand.far, offHand.farOff, offHand.farEnd = false, nil, nil
	offHand.restore(true)
end

-- ---------------------------------------------------------------------------
-- Laser de la main libre (gauche ; droite en mode gaucher), comme celui de la main qui tient
-- l'outil : le même trait orange depuis la visée de la manette, une croix au point touché.
-- Réglage « Laser de la main gauche ». Montré à pied, manette suivie, hors carte et hors vue
-- de la caméra d'un mod loin du joueur (comme le bras) ; caché quand la main tient un objet.
-- Il s'arrête sur nos panneaux : réglages, inventaire VR quand le joueur le regarde, fenêtre
-- d'un mod quand il la touche. La DLL envoie la distance où son rayon les touche
-- (" LAS <réglage> <distance>" : mètres de la pièce depuis la visée, -1 = aucun) ; le point
-- dessiné sur le panneau montre ce qu'elle choisit, sa gâchette choisit (Input::PointerHand).
-- Réglages ouverts : seulement quand il les vise.
-- Attraper de loin (main physique, réglage « attraper avec le grip »), comme le grip de
-- l'autre main (l'attrape du jeu), avec ses règles (voir FARMASS plus haut) : un objet mobile
-- (canFar) touché par le laser à moins de LRANGE est entouré de blanc ; le laser s'éclaire et
-- l'icône de main du jeu (ICON) est posée, à moitié transparente, au point visé. Le grip le
-- prend alors là où il est (grabFar) : il reste au bout du laser, à sa distance, et suit la
-- main avec la force de l'attrape du jeu (un objet très lourd se traîne ; trop loin de sa
-- place, il est lâché) ; il traverse le joueur (couche de la main, voir claim) ; grip relâché,
-- il part avec sa vitesse (lancer). Pendant ce temps le laser va jusqu'au point tenu, l'icône
-- dessus. Le grip de l'autre main reste l'attrape du jeu : elle n'est jamais visée.
-- Calculé sur les images normales ; l'image de l'œil droit (même instant) redessine les mêmes
-- traits et le même contour. Appelé par tdvrOffArmTick (fonction globale : tick() n'a plus de
-- place pour un upvalue).
-- ---------------------------------------------------------------------------

-- Un trait du laser (gardé pour l'image de l'œil droit).
offHand.line = function(p0, p1, r, g, b, a)
	DrawLine(p0, p1, r, g, b, a)
	local d = offHand.las
	d[#d + 1] = { p0, p1, r, g, b, a }
end

-- Croix de demi-taille s au point c.
offHand.cross = function(c, s, r, g, b)
	local o = offHand
	o.line(VecAdd(c, Vec(-s, 0, 0)), VecAdd(c, Vec(s, 0, 0)), r, g, b, 1)
	o.line(VecAdd(c, Vec(0, -s, 0)), VecAdd(c, Vec(0, s, 0)), r, g, b, 1)
	o.line(VecAdd(c, Vec(0, 0, -s)), VecAdd(c, Vec(0, 0, s)), r, g, b, 1)
end

-- L'objet b peut-il être pris de loin ? Comme l'attrape du jeu : mobile, de FARMASS au plus (une
-- porte aussi : elle tourne sur ses gonds ; un véhicule aussi) ; ni fait de trop de formes
-- (FARSHAPES), ni ce que porte le jeu, ni l'outil ou sa copie, ni la main, ni ce sur quoi le
-- joueur est debout ; ni un corps provisoire (identifiant négatif : le jeu ne les prend pas).
offHand.canFar = function(b)
	local o = offHand
	if b <= 0 or b == o.body or b == o.held or b == phys.body or b == GetToolBody() or b == GetPlayerGrabBody() then
		return false
	end
	local m = GetBodyMass(b)
	if not IsBodyDynamic(b) or m <= 0 or m > o.FARMASS or #(GetBodyShapes(b) or {}) > o.FARSHAPES then
		return false
	end
	local gc, gs = GetPlayerGroundContact()
	return not (gc and gs and gs ~= 0 and GetShapeBody(gs) == b)
end

-- L'icône de main du jeu (son viseur pendant l'attrape : data/ui/hud/crosshair-hand.png,
-- 50 x 64) au point c, à d mètres de la main, tournée vers les yeux (eye), dessinée par-dessus
-- tout comme le viseur du jeu ; sa taille suit la distance (même taille vue, ~3°, comme
-- l'icône à l'écran) ; a : opacité (visé : à moitié ; tenu : comme le jeu). L'image est blanche :
-- une ombre noire décalée en bas à droite (un peu plus loin des yeux : dessinée dessous) la garde
-- visible sur un objet clair. Gardée pour l'image de l'œil droit. Image introuvable : une croix
-- verte.
offHand.icon = function(c, d, eye, a)
	local o = offHand
	if o.sprite == nil then
		local ok, h = pcall(LoadSprite, o.ICON)
		o.sprite = ok and h or 0
	end
	if not o.sprite or o.sprite == 0 then
		o.cross(c, 0.018 * math.max(1, d * 0.6), 0.3, 1, 0.4)
		return
	end
	local h = math.max(0.035, 0.05 * VecLength(VecSub(c, eye)))
	local rot = QuatLookAt(c, eye)
	local sh = VecAdd(VecAdd(c, QuatRotateVec(rot, Vec(h * 0.07, -h * 0.07, 0))), VecScale(VecNormalize(VecSub(c, eye)), 0.02))
	o.spr = { Transform(c, rot), h * 50 / 64, h, a, Transform(sh, rot) }
	o.drawIcon(o.spr)
end

-- L'icône gardée (o.spr) : l'ombre, puis l'image.
offHand.drawIcon = function(sp)
	local o = offHand
	DrawSprite(o.sprite, sp[5], sp[2], sp[3], 0, 0, 0, sp[4] * 0.8, false, false)
	DrawSprite(o.sprite, sp[1], sp[2], sp[3], 1, 1, 1, sp[4], false, false)
end

-- allowed : comme le bras (à pied, ni carte ni caméra d'un mod loin du joueur).
function tdvrOffLaserTick(st, toWorld, frozen, allowed)
	local o = offHand
	if frozen then
		-- (œil droit, même instant : les traits et le contour de l'œil gauche)
		local d = o.las
		if d then
			for i = 1, #d do
				local l = d[i]
				DrawLine(l[1], l[2], l[3], l[4], l[5], l[6])
			end
			if o.lasBody ~= 0 and IsHandleValid(o.lasBody) then DrawBodyOutline(o.lasBody, 1, 1, 1, 0.7) end
			local sp = o.spr
			if sp and o.sprite and o.sprite ~= 0 then o.drawIcon(sp) end
		end
		return
	end
	o.las, o.lasBody, o.lt, o.ltPoint, o.ltAim, o.panel, o.spr, o.lasHit = nil, 0, 0, nil, nil, false, nil, 0
	o.lasTarget = nil   -- (point visé par ce laser dans cette image : l'arme de cette main y tire, voir tdvrDualTick)
	-- Réponse de la DLL : " LAS <réglage> <distance du panneau visé>" ; " ARM ... <attraper> ..."
	local on, pd, canGrab = false, -1, false
	if fastLua.tagsOk then
		-- (fastLua : étiquettes déjà repérées, voir fastLua.index)
		local jl, ja = fastLua.tag("LAS", 100, #tokens - 2), fastLua.tag("ARM", 100, #tokens - 2)
		if jl then on, pd = tokens[jl + 1] == "1", tonumber(tokens[jl + 2]) or -1 end
		if ja then canGrab = tokens[ja + 3] == "1" end
	else
		for j = 100, #tokens - 2 do
			local tk = tokens[j]
			if tk == "LAS" then
				on, pd = tokens[j + 1] == "1", tonumber(tokens[j + 2]) or -1
			elseif tk == "ARM" then
				canGrab = tokens[j + 3] == "1"
			end
		end
	end
	-- Armes doubles (tdvrDualTick) : cette main tient une arme. Elle vise toujours (réglage coupé :
	-- seulement une croix au point visé) et n'attrape rien de loin (son grip fait s'accroupir).
	local dual = tdvrDual and tdvrDual.on
	local cross = dual and not on
	if dual then on, canGrab = true, false end
	o.panel = on and pd >= 0
	local right = st.leftHanded == 1   -- (la main libre est la droite en mode gaucher)
	local tracked = math.floor(st.buttons / (right and 256 or 128)) % 2 == 1
	-- (caché quand la main tient un objet dans la main ; tenu de loin, il va jusqu'à lui)
	if not (on and allowed and tracked and GetPlayerHealth() > 0) or (o.held ~= 0 and not o.far) then return end
	local aim = toWorld(right and st.raim or st.laim)
	local dir = QuatRotateVec(aim.rot, Vec(0, 0, -1))
	local from = VecAdd(aim.pos, VecScale(dir, 0.08))
	local s = st.worldScale
	local eye = toWorld(st.head).pos
	if o.held ~= 0 and IsHandleValid(o.held) and o.hold then
		-- Objet tenu de loin : le laser va jusqu'au point tenu, l'icône de main dessus.
		o.las = {}
		local c = TransformToParentPoint(GetBodyTransform(o.held), o.hold.pos)
		o.line(from, c, 1, 0.8, 0.35, 0.8)
		o.icon(c, VecLength(VecSub(c, aim.pos)), eye, 0.75)
		o.logLaser("holding", VecLength(VecSub(c, aim.pos)), o.held)
		return
	end
	if st.uiOpen == 1 and pd < 0 then return end   -- (réglages ouverts : seulement quand il les vise)
	o.las = {}
	if pd >= 0 then
		-- Sur un panneau : arrêté dessus (le panneau dessine son propre point).
		if pd * s > 0.1 and not cross then o.line(from, VecAdd(aim.pos, VecScale(dir, pd * s)), 1, 0.55, 0.1, 0.5) end
		o.logLaser("panel", pd * s, 0)
		return
	end
	QueryRequire("physical visible")
	local tb = GetToolBody()
	if tb ~= 0 then QueryRejectBody(tb) end
	rejectPhys()
	if QueryRejectPlayer then pcall(QueryRejectPlayer) end
	local hit, dist, _, shape = QueryRaycast(aim.pos, dir, 150)
	if not hit then dist = 150 end
	o.lasTarget = VecAdd(aim.pos, VecScale(dir, dist))
	if cross then
		-- (armes doubles, laser gauche coupé : une croix au point visé, comme au bout du laser)
		if hit then o.cross(o.lasTarget, 0.012 * math.max(1, dist * 0.6), 1, 0.6, 0.15) end
		o.logLaser(hit and "hit (dual wield, cross only)" or "sky", dist, 0)
		return
	end
	-- Cible à attraper (main physique présente) : l'objet touché, sinon un objet frôlé par un
	-- rayon plus épais (petits objets) avant le premier obstacle (sans le dépasser : jamais un
	-- objet derrière un mur ou une vitre).
	local target, td = 0, dist
	if canGrab and o.body ~= 0 then
		local range = o.LRANGE * s
		if hit and dist <= range and shape ~= 0 and o.canFar(GetShapeBody(shape)) then
			target = GetShapeBody(shape)
		elseif not hit or dist > 0.1 then
			QueryRequire("physical dynamic visible")
			if tb ~= 0 then QueryRejectBody(tb) end
			rejectPhys()
			if QueryRejectPlayer then pcall(QueryRejectPlayer) end
			local h2, d2, _, s2 = QueryRaycast(aim.pos, dir, math.min(range, dist - 0.05), 0.06)
			if h2 and s2 ~= 0 and o.canFar(GetShapeBody(s2)) then target, td = GetShapeBody(s2), d2 end
		end
	end
	o.lt = target
	if target ~= 0 then
		-- Objet à attraper : le laser s'arrête dessus, éclairé, l'icône de main du jeu au point
		-- visé (là où le grip le prendra), contour blanc.
		local c = VecAdd(aim.pos, VecScale(dir, td))
		if td < dist then
			-- (rayon épais : c est le centre de la sphère au contact, ~6 cm hors de l'objet :
			-- ramené sur l'objet, pour l'icône et le point tenu)
			local okc, cp = GetBodyClosestPoint(target, c)
			if okc then c = cp end
		end
		o.ltPoint, o.ltAim, o.ltS = c, aim, s
		if td > 0.1 then o.line(from, c, 1, 0.8, 0.35, 0.8) end
		o.icon(c, td, eye, 0.45)
		o.lasBody = target
		DrawBodyOutline(target, 1, 1, 1, 0.7)
		o.logLaser("target", td, target)
		return
	end
	o.line(from, VecAdd(aim.pos, VecScale(dir, math.min(dist, 8))), 1, 0.55, 0.1, 0.35)
	if hit then
		o.cross(VecAdd(aim.pos, VecScale(dir, dist)), 0.012 * math.max(1, dist * 0.6), 1, 0.6, 0.15)
	end
	o.lasHit = hit and shape ~= 0 and GetShapeBody(shape) or 0   -- (journal : ce qui est touché)
	o.logLaser(hit and "hit" or "sky", dist, 0)
end

-- Diagnostic (debugLua) : état du laser, une fois par seconde.
offHand.logLaser = function(what, dist, target)
	local o = offHand
	if not camDebug then return end
	o.lasLogT = (o.lasLogT or 0) + lastTickDt
	if o.lasLogT < 1 or debugMsg then return end
	o.lasLogT = 0
	local hb = target == 0 and o.lasHit or 0
	o.lasHit = 0
	local hinfo = ""
	if hb ~= 0 and IsHandleValid(hb) then
		hinfo = string.format(", hit body %d (%.1f kg, dynamic %s, %d shapes)", hb, GetBodyMass(hb),
			tostring(IsBodyDynamic(hb)), #(GetBodyShapes(hb) or {}))
	end
	debugMsg = string.format("offlaser: %s at %.2f m, target %d (%.1f kg), icon %s, lines %d%s", what, dist, target,
		target ~= 0 and GetBodyMass(target) or 0, tostring(o.spr ~= nil), o.las and #o.las or 0, hinfo)
end

-- Casque simulé seulement (B + gâchette gauche, voir mockPushTest) : essais de la main
-- physique. Une dalle fixe de 1,2 x 0,1 x 1,2 m dont le dessus est 4 cm sous le poing, et
-- une caisse de 50 cm posée dessus, sa face avant 15 cm devant le centre du poing (hors de
-- portée d'attrape : il faut d'abord avancer jusqu'à elle, ou la prendre de loin au laser) ; en bois
-- (~9 kg), puis en métal (lourde) à l'appui suivant, puis une grande caisse de métal de
-- 1 x 0,5 x 1 m (essai de force : bien trop lourde pour l'ancienne main de 600 N), et ainsi
-- de suite. Toutes deux sur la couche 64, que le joueur
-- ignore pendant les essais : seule la main les touche.
function tdvrMockOffHandScene()
	local w = offHand.want
	if not w then return end
	for _, h in ipairs(mockCrate.lh or {}) do
		if h ~= 0 and IsHandleValid(h) then Delete(h) end
	end
	local yaw = QuatEuler(0, yawOf(GetCameraTransform(1).rot), 0)   -- (1 : vraie caméra, la tête)
	local function spawnBody(xml, pos, static)
		for _, h in ipairs(Spawn(xml, Transform(VecAdd(w.pos, QuatRotateVec(yaw, pos)), yaw), static, false) or {}) do
			if GetEntityType(h) == "body" then return h end
		end
		return 0
	end
	local slab = spawnBody('<body tags="tdvr_testslab nomerge" dynamic="false"><voxbox size="12 1 12" material="masonry"/></body>',
		Vec(-0.6, -0.19, -0.9), true)
	for _, s in ipairs(slab ~= 0 and GetBodyShapes(slab) or {}) do SetShapeCollisionFilter(s, 64, 255) end
	mockCrate.lhStep = mockCrate.lh and (mockCrate.lhStep or 1) % 3 + 1 or 1
	local big = mockCrate.lhStep == 3
	local crate = spawnBody(string.format('<body tags="tdvr_testcrate" dynamic="true"><voxbox size="%s" material="%s"/></body>',
		big and "10 5 10" or "5 5 5", mockCrate.lhStep == 1 and "wood" or "metal"), Vec(big and -0.5 or -0.25, -0.09, big and -1.15 or -0.65), false)
	for _, s in ipairs(crate ~= 0 and GetBodyShapes(crate) or {}) do SetShapeCollisionFilter(s, 64, 255) end
	mockCrate.lh = { slab, crate }
	mockCrate.lhP0 = crate ~= 0 and GetBodyTransform(crate).pos or nil
	local m = GetPlayerParam("CollisionMask")
	if type(m) == "number" and math.floor(m / 64) % 2 == 1 then SetPlayerParam("CollisionMask", m - 64) end
	debugMsg = string.format("mock: off-hand test scene, slab %d, crate %d (mass %.1f kg), hand %.2f %.2f %.2f",
		slab, crate, crate ~= 0 and GetBodyMass(crate) or 0, w.pos[1], w.pos[2], w.pos[3])
end

-- ---------------------------------------------------------------------------
-- Armes doubles : une arme dans chaque main (réglage « Armes doubles », coché par défaut).
-- Total Overdose (workshop 3449723355, outils « totaloverdose » et « totaloverdose2 ») dessine
-- ses deux pistolets dans UN outil, tenu par la main droite, et tire avec un seul bouton (clic
-- gauche), une fois l'un, une fois l'autre, depuis l'œil du joueur. En VR :
--   - la DLL dit que l'outil en main est une arme double (profil, réglage, à pied, visée à la
--     manette) : réponse " DUA <permis> <tirs> <arme du dernier tir> <prêt>" ;
--   - ce script coupe l'outil en deux, de gauche à droite dans la vue écran du mod (le plus grand
--     vide entre ses voxels), garde le côté de la main du mod et efface les voxels de l'autre côté
--     (gardés : remis quand les armes doubles s'arrêtent sur ce même outil). La main dominante
--     tient le pistolet gardé (l'outil du jeu : recul du mod, copie physique, collisions), par sa
--     poignée (voxels de l'arrière et du bas du pistolet) ;
--   - l'autre main tient une copie de ce pistolet (corps fixe sans collision, faite comme la copie
--     physique : CopyShapeContent...), placée comme la DLL place l'outil dans la main dominante
--     (main x H⁻¹ x pose écran du mod au repos), sur la main dessinée (tdvrOffArmTick : contre un
--     mur, elle reste dans la main physique) ; elle a son propre recul ;
--   - pour chaque arme (1 = main dominante, 0 = l'autre), il envoie la bouche du canon dans le
--     repère de son corps (TDVRG) et le point visé par son laser moins cette bouche (TDVRK, monde) ;
--     pour la copie, son corps (TDVRV) et l'œil moins sa position (TDVRX : recalage au rendu,
--     comme le bras) ;
--   - la DLL fait de chaque appui de gâchette un clic, l'un après l'autre, et donne au mod pour ce
--     tir des « yeux » qui mettent sa balle sur la bouche de l'arme de cette main, vers sa cible
--     (engine.cpp, DualEye ; input.cpp, DualClicks).
-- Pendant ce temps (main libre : la gauche, la droite en mode gaucher) : sa gâchette = son arme, son
-- grip = s'accroupir (la main tient une arme). Copie cachée (et gâchette de la main libre sans effet)
-- quand cette main tient un objet ou que sa manette n'est plus suivie. Si l'outil ne se coupe pas en deux armes, ou si l'effacement est sans
-- effet, rien ne change (commandes habituelles) : journal « Lua: dual: ... ».
-- Appelé par tick() (fonction globale : tick() n'a plus de place pour un upvalue).
-- ---------------------------------------------------------------------------
-- (nonserializable : le jeu ne met pas cette table dans ses sauvegardes rapides - recharger une partie
-- y remettrait sinon l'état d'alors : poignées, voxels effacés, copie ; elle repart de zéro comme les
-- variables locales du mod)
tdvrDual = { nonserializable = true, on = false, body = 0, key = nil, copy = 0, copyShapes = {}, shown = false,
	erased = nil, failed = {}, H = nil, prevH = nil, M = nil, rest = nil, seq = nil, kick = 0, domShotT = 9, offT = 0,
	waitT = 0, sendT = 0, logT = 0, rTarget = nil }
tdvrDual.GRIPinv = TransformToLocalTransform(offArm.GRIP, Transform())   -- (main dessinée -> poignée de la manette)

-- Ligne du journal de la DLL (« Lua: dual: ... »). (Long : envoyé à part, il peut se perdre.)
tdvrDual.log = function(s)
	pcall(SetClipboardText, "TDVRL dual: " .. s)
end

-- Message court (30 caractères au plus, voir exchange) : en-tête + vecteur, précision selon sa taille.
tdvrDual.send = function(head, v)
	local m = math.max(math.abs(v[1]), math.abs(v[2]), math.abs(v[3]))
	local f = m < 9.99 and "%s %.3f %.3f %.3f" or (m < 99.9 and "%s %.2f %.2f %.2f" or (m < 999 and "%s %.1f %.1f %.1f" or nil))
	if not f then return false end
	local s = string.format(f, head, v[1], v[2], v[3])
	if #s > 30 then return false end
	pcall(SetClipboardText, s)
	return true
end

-- Réponse de la DLL : " DUA <permis> <tirs> <arme du dernier tir> <prêt>".
tdvrDual.parse = function()
	local from, to = 100, #tokens - 4
	if fastLua.tagsOk then
		local j = fastLua.tag("DUA", from, to)   -- (fastLua : étiquette déjà repérée)
		from, to = j or 1, j or 0
	end
	for j = from, to do
		if tokens[j] == "DUA" then
			return tokens[j + 1] == "1", tonumber(tokens[j + 2]) or 0, tonumber(tokens[j + 3]) or -1, tonumber(tokens[j + 4]) or 0
		end
	end
	return false, 0, -1, 0
end

-- Les voxels pleins de l'outil (formes à moins de 10 m de son corps ; 65 536 cases lues au plus) :
-- forme, indices, matière (indice de palette), taille du voxel, centre dans le repère du corps (b)
-- et dans la vue écran du mod (v, repère de l'œil), tels qu'ils sont dessinés. nil : trop gros.
tdvrDual.scan = function(body, flat)
	local list, cells = {}, 0
	for _, sh in ipairs(GetBodyShapes(body) or {}) do
		local sx, sy, sz, s = GetShapeSize(sh)
		local lt = GetShapeLocalTransform(sh, 1)   -- (1 : la forme telle qu'elle est dessinée)
		if sx and sx > 0 and s and s > 0 and VecLength(lt.pos) < 10 and GetShapeVoxelCount(sh) > 0 then
			cells = cells + sx * sy * sz
			if cells > 65536 then return nil end
			for z = 0, sz - 1 do
				for y = 0, sy - 1 do
					for x = 0, sx - 1 do
						local _, _, _, _, _, e = GetShapeMaterialAtIndex(sh, x, y, z)
						if e and e ~= 0 then
							local b = TransformToParentPoint(lt, Vec((x + 0.5) * s, (y + 0.5) * s, (z + 0.5) * s))
							list[#list + 1] = { sh = sh, x = x, y = y, z = z, e = e, s = s, b = b, v = TransformToParentPoint(flat, b) }
						end
					end
				end
			end
		end
	end
	return list
end

-- Pièces : voxels d'une même forme qui se touchent par une face. Chaque voxel reçoit le volume de
-- sa pièce (vol : les repères de 1 cm du mod sont de toutes petites pièces).
tdvrDual.islands = function(list)
	local at, seen = {}, {}
	for i, p in ipairs(list) do at[p.sh .. ":" .. p.x .. ":" .. p.y .. ":" .. p.z] = i end
	for i in ipairs(list) do
		if not seen[i] then
			local stack, members, vol = { i }, {}, 0
			seen[i] = true
			while #stack > 0 do
				local q = list[table.remove(stack)]
				members[#members + 1] = q
				vol = vol + q.s * q.s * q.s
				for k = 1, 6 do
					local a, sg = math.floor((k - 1) / 2) + 1, (k % 2 == 1) and 1 or -1
					local j = at[q.sh .. ":" .. (q.x + (a == 1 and sg or 0)) .. ":" .. (q.y + (a == 2 and sg or 0)) .. ":"
						.. (q.z + (a == 3 and sg or 0))]
					if j and not seen[j] then
						seen[j] = true
						stack[#stack + 1] = j
					end
				end
			end
			for _, q in ipairs(members) do q.vol = vol end
		end
	end
end

-- La coupure : le plus grand vide de gauche à droite (x de la vue écran) entre les voxels, d'au
-- moins 4 cm, avec au moins un quart du volume de chaque côté. Sinon nil (et ce vide) : pas deux armes.
tdvrDual.cut = function(list)
	if #list < 2 then return nil, 0 end
	table.sort(list, function(a, b) return a.v[1] < b.v[1] end)
	local best, at, reach = 0, nil, list[1].v[1] + list[1].s * 0.5
	for i = 2, #list do
		local p = list[i]
		local gap = p.v[1] - p.s * 0.5 - reach
		if gap > best then best, at = gap, reach + gap * 0.5 end
		reach = math.max(reach, p.v[1] + p.s * 0.5)
	end
	if not at or best < 0.04 then return nil, best end
	local l, t = 0, 0
	for _, p in ipairs(list) do
		local w = p.s * p.s * p.s
		t = t + w
		if p.v[1] < at then l = l + w end
	end
	if l < 0.25 * t or t - l < 0.25 * t then return nil, best end
	return at, best
end

-- Efface les voxels de l'autre côté de la coupure (side : 1 = la droite est gardée, -1 la gauche).
-- Renvoie les effacés, les gardés, et combien sont vraiment vides ensuite (relus un par un).
tdvrDual.erase = function(list, cut, side)
	local er, kept = {}, {}
	SetBrush("cube", 1, 0)   -- (matière 0 : retire les voxels)
	for _, p in ipairs(list) do
		if (p.v[1] - cut) * side < 0 then
			DrawShapeBox(p.sh, p.x, p.y, p.z, p.x, p.y, p.z)
			er[#er + 1] = p
		else
			kept[#kept + 1] = p
		end
	end
	local gone = 0
	for _, p in ipairs(er) do
		local e = 0
		if IsHandleValid(p.sh) then e = select(6, GetShapeMaterialAtIndex(p.sh, p.x, p.y, p.z)) end
		if not e or e == 0 then gone = gone + 1 end
	end
	return er, kept, gone
end

-- Remet les voxels effacés (même matière) sur les formes encore là. Renvoie leur nombre.
tdvrDual.restore = function()
	local d = tdvrDual
	local n = 0
	for _, p in ipairs(d.erased or {}) do
		if IsHandleValid(p.sh) then
			SetBrush("cube", 1, p.e)
			DrawShapeBox(p.sh, p.x, p.y, p.z, p.x, p.y, p.z)
			n = n + 1
		end
	end
	d.erased = nil
	return n
end

-- Poignée (repère de l'œil : centre des voxels de l'arrière - 30 % de la longueur - et du bas - 60 %
-- de la hauteur) et bouche (centre des voxels les plus en avant, posé sur leur face avant) de l'arme
-- gardée, et sa longueur ; les toutes petites pièces (moins de 10 % du volume) ne comptent pas.
tdvrDual.measure = function(kept)
	local tot, big = 0, {}
	for _, p in ipairs(kept) do tot = tot + p.s * p.s * p.s end
	for _, p in ipairs(kept) do
		if (p.vol or 0) >= 0.1 * tot then big[#big + 1] = p end
	end
	if #big == 0 then big = kept end
	local minZ, maxZ, minY, maxY = math.huge, -math.huge, math.huge, -math.huge
	for _, p in ipairs(big) do
		local h = p.s * 0.5
		minZ, maxZ = math.min(minZ, p.v[3] - h), math.max(maxZ, p.v[3] + h)
		minY, maxY = math.min(minY, p.v[2] - h), math.max(maxY, p.v[2] + h)
	end
	local lz, ly = maxZ - minZ, maxY - minY
	local g, gn, m, mn = Vec(0, 0, 0), 0, Vec(0, 0, 0), 0
	for _, p in ipairs(big) do
		if p.v[3] >= maxZ - 0.3 * lz and p.v[2] <= minY + 0.6 * ly then
			g = VecAdd(g, p.v)
			gn = gn + 1
		end
		if p.v[3] <= minZ + p.s then
			m = VecAdd(m, p.v)
			mn = mn + 1
		end
	end
	if gn == 0 then
		for _, p in ipairs(big) do g = VecAdd(g, p.v) end
		gn = #big
	end
	g = VecScale(g, 1 / gn)
	m = VecScale(m, 1 / math.max(mn, 1))
	m[3] = minZ
	return g, m, lz
end

-- Coupe l'outil (une fois par outil et par taille). Renvoie nil et un texte (réussi), ou la raison
-- de l'échec (rien n'est alors changé).
tdvrDual.split = function(st, body)
	local d = tdvrDual
	local flat = st.toolFlat
	local list = d.scan(body, flat)
	if not list then return "tool too big to read" end
	if #list < 2 then return "tool has no voxels" end
	d.islands(list)
	local cut, gap = d.cut(list)
	if not cut then return string.format("no two guns side by side (largest gap %.3f m, %d voxels)", gap or 0, #list) end
	-- (le côté de la main du mod sur son outil ; tout près de la coupure : la droite)
	local hx = modHold.H and modHold.H.pos[1] or 1
	local side = hx < cut - 0.03 and -1 or 1
	local er, kept, gone = d.erase(list, cut, side)
	d.erased = er
	if gone < #er or #kept == 0 then
		d.restore()
		return string.format("erasing the other gun had no effect (%d of %d voxels gone) - fallback: both guns stay "
			.. "in the dominant hand", gone, #er)
	end
	local g, m, len = d.measure(kept)
	d.prevH = modHold.H
	d.H = Transform(g)
	d.M = TransformToLocalPoint(flat, m)
	d.rest = flat
	-- La main dominante tient le pistolet gardé par sa poignée (outil, copie physique et sphères
	-- refaits : la prise change).
	modHold.H, modHold.ver, modHold.since, modHold.how = d.H, modHold.ver + 1, 0, "dual wield grip"
	physDestroy()
	return nil, string.format("kept the %s gun (cut at x %.3f, gap %.3f m), erased %d of %d voxels; grip %.3f %.3f %.3f "
		.. "(desktop view), muzzle %.3f %.3f %.3f in the tool body, gun %.2f m long", side > 0 and "right" or "left", cut,
		gap, #er, #list, g[1], g[2], g[3], d.M[1], d.M[2], d.M[3], len)
end

-- La copie : un corps fixe, une forme par forme de l'outil qui a encore des voxels (même contenu,
-- même palette, même taille de voxel, même place dans le corps : telle qu'elle est dessinée), sans
-- collision ni dégâts, toujours dessinée.
tdvrDual.build = function(body, pose)
	local d = tdvrDual
	d.destroyCopy()
	for _, b in ipairs(FindBodies("tdvr_dual", true) or {}) do Delete(b) end   -- (orphelins : sauvegarde rechargée)
	local list = {}
	local parts = { '<body tags="tdvr_dual nomerge" dynamic="false">' }
	for _, sh in ipairs(GetBodyShapes(body) or {}) do
		local sx, _, _, s = GetShapeSize(sh)
		if sx and sx > 0 and s and s > 0 and #list < 8 and GetShapeVoxelCount(sh) > 0
			and VecLength(GetShapeLocalTransform(sh, 1).pos) < 10 then
			list[#list + 1] = sh
			parts[#parts + 1] = string.format('<vox file="MOD/proxy.vox" scale="%.4f" tags="tdvr_part=%d unbreakable nocull" '
				.. 'collide="false"/>', s * 10, #list)
		end
	end
	if #list == 0 then return false end
	parts[#parts + 1] = '</body>'
	local b, vis = 0, {}
	for _, h in ipairs(Spawn(table.concat(parts), pose, true, false) or {}) do
		local ty = GetEntityType(h)
		if ty == "body" and b == 0 then
			b = h
		elseif ty == "shape" then
			local i = tonumber(GetTagValue(h, "tdvr_part"))
			if i then vis[i] = h end
		end
	end
	if b == 0 then return false end
	local shapes = {}
	for i, sh in ipairs(list) do
		local v = vis[i]
		if v then
			CopyShapeContent(sh, v)
			CopyShapePalette(sh, v)
			SetShapeLocalTransform(v, GetShapeLocalTransform(sh, 1))   -- (1 : telle qu'elle est dessinée)
			SetShapeCollisionFilter(v, 0, 0)   -- (ne touche rien, rien ne la touche)
			shapes[#shapes + 1] = v
		end
	end
	if #shapes == 0 then
		Delete(b)
		return false
	end
	d.copy, d.copyShapes, d.shown, d.sendT = b, shapes, true, 0
	return true
end

tdvrDual.destroyCopy = function()
	local d = tdvrDual
	if d.copy ~= 0 then
		if IsHandleValid(d.copy) then Delete(d.copy) end
		pcall(SetClipboardText, "TDVRV 0")
	end
	d.copy, d.copyShapes, d.shown = 0, {}, false
end

tdvrDual.show = function(on)
	local d = tdvrDual
	if on == d.shown or d.copy == 0 then return end
	d.shown = on
	for _, s in ipairs(d.copyShapes) do
		if IsHandleValid(s) then
			if on then RemoveTag(s, "invisible") else SetTag(s, "invisible") end
		end
	end
end

-- Fin des armes doubles sur cet outil : voxels remis (même outil), copie supprimée, prise d'origine.
tdvrDual.stop = function(why)
	local d = tdvrDual
	local n = d.restore()
	d.destroyCopy()
	if d.H and modHold.H == d.H then
		modHold.H, modHold.ver, modHold.since = d.prevH, modHold.ver + 1, 0
	end
	if n > 0 then physDestroy() end
	d.log(string.format("stopped (%s): %d voxels put back", why, n))
	d.body, d.key, d.H, d.prevH, d.M, d.rest, d.on, d.kick, d.waitT = 0, nil, nil, nil, nil, nil, false, 0, 0
end

tdvrDual.run = function(st, toWorld, allowed)
	local d = tdvrDual
	local dt = lastTickDt
	local ok, shots, gun, ready = d.parse()
	local body = GetToolBody()
	local want = ok and allowed and st.aimMode == 0 and st.toolKind == 2 and body ~= 0
	if want then d.offT = 0 else d.offT = d.offT + dt end
	-- Autre outil (ou le même refait), autre taille, autre point de réduction de la DLL (les formes
	-- bougent dans le corps) : tout est refait.
	local key = body .. ":" .. (modHold.keyScale or "") .. ":" .. (st.toolPivotKey or "")
	if d.body ~= 0 and (body ~= d.body or key ~= d.key) then
		d.stop(body ~= d.body and "tool changed" or "tool size changed")
	end
	if not want then
		-- (coupé un moment - réglage décoché, véhicule, carte, caméra d'un mod, 0,5 s - : l'outil
		-- retrouve son autre arme ; un instant seulement : la copie est juste cachée)
		if d.body ~= 0 and d.offT > 0.5 then
			d.stop(ok and "not on foot, or not aiming with the controller" or "not allowed (setting, tool, mod camera)")
		end
		d.show(false)
		d.on, d.seq, d.rTarget = false, nil, nil
		return
	end
	-- Coupe : une fois l'outil posé (prise connue et dessinée, 0,5 s après).
	if d.body == 0 then
		d.on, d.rTarget = false, nil
		if d.failed[key] then return end
		-- (pas pendant un tir : la pose écran du mod, qui sert à la coupe, est alors relevée par son recul)
		if modHold.keyBody ~= body or not modHold.H or modHold.since < 2 or InputDown("usetool") then
			d.waitT = 0
			return
		end
		d.waitT = d.waitT + dt
		if d.waitT < 0.5 then return end
		local err, msg = d.split(st, body)
		if err then
			d.failed[key] = true
			d.log(string.format("%s: %s - controls as usual", GetString("game.player.tool"), err))
			return
		end
		d.body, d.key, d.seq, d.domShotT, d.kick = body, key, nil, 9, 0
		d.log(GetString("game.player.tool") .. ": " .. msg)
	end
	-- Tirs (DLL) : la main qui a tiré vibre ; la copie a son propre recul. Celui de l'arme dominante
	-- (le recul du mod) ne passe pas dans la pose de repos de la copie (0,25 s après son tir).
	if d.seq and shots ~= d.seq and gun >= 0 then
		if (gun == 1) == (st.leftHanded ~= 1) then hapR = math.max(hapR, 0.6) else hapL = math.max(hapL, 0.6) end
		if gun == 0 then d.kick = 20 else d.domShotT = 0 end
	end
	d.seq = shots
	d.domShotT = d.domShotT + dt
	if d.domShotT > 0.25 then d.rest = st.toolFlat end
	d.kick = d.kick * math.exp(-60 * dt)
	if d.kick < 0.2 then d.kick = 0 end
	-- La copie dans l'autre main : sa main dessinée (tdvrOffArmTick ; sinon la manette), tournée
	-- comme sa visée, décalée comme l'outil (réglage, en miroir), tenue au même point H que l'outil.
	local right = st.leftHanded == 1   -- (la main libre est la droite en mode gaucher)
	local tracked = math.floor(st.buttons / (right and 256 or 128)) % 2 == 1
	local raw = toWorld(right and st.rgrip or st.lgrip)
	local aim = toWorld(right and st.raim or st.laim)
	local grip = offHand.drawnIk and TransformToParentTransform(offHand.drawnIk, d.GRIPinv) or raw
	local hand = Transform(grip.pos, QuatRotateQuat(QuatRotateQuat(grip.rot, quatConj(raw.rot)), aim.rot))
	local off = st.toolOff
	local target = TransformToParentTransform(hand, Transform(Vec(-off[1], off[2], off[3]), QuatEuler(st.toolPitch + d.kick, 0, 0)))
	local pose = TransformToParentTransform(TransformToParentTransform(target, TransformToLocalTransform(d.H, Transform())), d.rest)
	if d.copy == 0 or not IsHandleValid(d.copy) then
		if not d.build(body, pose) then
			d.failed[key] = true
			d.stop("the copy could not be made")
			return
		end
		d.log(string.format("copy body %d (%d shapes) in the %s hand", d.copy, #d.copyShapes, right and "right" or "left"))
		pcall(SetClipboardText, "TDVRV " .. d.copy)
		d.sendT = 60
	end
	SetBodyTransform(d.copy, pose)
	local show = tracked and offHand.held == 0 and GetPlayerHealth() > 0
	d.show(show)
	d.on = true
	-- Arme 1 (main dominante) : l'outil du jeu, vers la cible de son laser (updateAim, cette image).
	local lh = st.leftHanded == 1
	local t1, v1 = d.rTarget, nil
	d.rTarget = nil
	if t1 then
		local af = QuatRotateVec(toWorld(lh and st.laim or st.raim).rot, Vec(0, 0, -1))
		v1 = VecSub(t1, TransformToParentPoint(GetBodyTransform(body), d.M))
		if VecDot(v1, af) < 0.3 then v1 = VecScale(af, 50) end   -- (cible sur l'arme ou derrière : droit devant)
		d.send("TDVRG 1", d.M)
		d.send("TDVRK 1", v1)
	end
	-- Arme 0 (l'autre main) : la copie, vers la cible de son laser (tdvrOffLaserTick ; sinon un rayon).
	local v0 = nil
	if show then
		local dir = QuatRotateVec(aim.rot, Vec(0, 0, -1))
		local t0 = offHand.lasTarget
		if not t0 then
			QueryRequire("physical visible")
			QueryRejectBody(body)
			rejectPhys()
			local hit, dist = QueryRaycast(aim.pos, dir, 150)
			t0 = VecAdd(aim.pos, VecScale(dir, hit and dist or 150))
		end
		v0 = VecSub(t0, TransformToParentPoint(GetBodyTransform(d.copy), d.M))
		if VecDot(v0, dir) < 0.3 then v0 = VecScale(dir, 50) end
		d.send("TDVRG 0", d.M)
		d.send("TDVRK 0", v0)
		local r = VecSub(toWorld(st.eyePose).pos, pose.pos)
		if math.abs(r[1]) < 9 and math.abs(r[2]) < 9 and math.abs(r[3]) < 9 then d.send("TDVRX", r) end
	end
	d.sendT = d.sendT - 1
	if d.sendT <= 0 then
		d.sendT = 60
		pcall(SetClipboardText, "TDVRV " .. d.copy)
	end
	if camDebug then
		d.logT = d.logT + dt
		if d.logT > 2 and not debugMsg then
			d.logT = 0
			local m0 = TransformToParentPoint(GetBodyTransform(d.copy), d.M)
			local m1 = TransformToParentPoint(GetBodyTransform(body), d.M)
			debugMsg = string.format("dual: on, copy %d shown %s, DLL ready %d, shots %d (last gun %d), kick %.1f, muzzles "
				.. "off %.2f %.2f %.2f -> %s m, dominant %.2f %.2f %.2f -> %s m, off hand drawn %s, crouch %.2f",
				d.copy, tostring(show), ready, shots, gun, d.kick, m0[1], m0[2], m0[3], v0 and string.format("%.1f", VecLength(v0)) or "-",
				m1[1], m1[2], m1[3], v1 and string.format("%.1f", VecLength(v1)) or "-", tostring(offHand.drawnIk ~= nil),
				GetPlayerCrouch())
		end
	end
end

function tdvrDualTick(st, toWorld, frozen, allowed)
	if frozen then return end   -- (œil droit : la copie reste où l'œil gauche l'a vue ; recalée par la DLL)
	local ok, err = pcall(tdvrDual.run, st, toWorld, allowed)
	if not ok then
		tdvrDual.errT = (tdvrDual.errT or 0) - lastTickDt
		if tdvrDual.errT <= 0 then
			tdvrDual.errT = 5
			tdvrDual.log("error: " .. tostring(err))
		end
	end
end

-- ---------------------------------------------------------------------------
-- Vibrations
-- ---------------------------------------------------------------------------
local function updateHaptics(st)
	local dom = st.leftHanded == 1 and 0 or 1
	local amp = 0
	-- (armes doubles : chaque gâchette clique ; la main qui a tiré vibre, voir tdvrDualTick)
	if tdvrDual and tdvrDual.on then
		-- (rien ici)
	elseif InputPressed("usetool") then amp = 0.6
	elseif InputDown("usetool") then amp = 0.12 end
	if InputPressed("grab") then amp = math.max(amp, 0.4) end
	local tool = GetString("game.player.tool")
	if tool ~= lastTool then
		lastTool = tool
		amp = math.max(amp, 0.25)
	end
	amp = math.max(amp, phys.hap or 0)
	phys.hap = 0
	if dom == 1 then hapR = amp else hapL = amp end
end

-- ---------------------------------------------------------------------------
function init()
	debugInfo = GetBool("savegame.mod.teardownvr.debug")
	-- (armes doubles : rien d'une partie d'avant ; sa copie orpheline est supprimée juste après)
	tdvrDual.body, tdvrDual.key, tdvrDual.copy, tdvrDual.copyShapes, tdvrDual.shown = 0, nil, 0, {}, false
	tdvrDual.erased, tdvrDual.H, tdvrDual.prevH, tdvrDual.M, tdvrDual.on, tdvrDual.seq = nil, nil, nil, nil, false, nil
	physCleanup()
	pcall(tdvrOffHandCleanup)   -- (main physique et objets tenus d'une partie rechargée)
end

function tick(dt)
	lastTickDt = dt
	tickCount = tickCount + 1
	if pendingExchangeLog and not debugMsg then
		debugMsg = pendingExchangeLog
		pendingExchangeLog = nil
	end
	if physMsg and not debugMsg then
		debugMsg = physMsg
		physMsg = nil
	end
	if aimMsg and not debugMsg then
		debugMsg = aimMsg
		aimMsg = nil
	end
	if stereoMsg and not debugMsg then
		debugMsg = stereoMsg
		stereoMsg = nil
	end
	if camBadDetail and not debugMsg then
		debugMsg = camBadDetail
		camBadDetail = nil
	end
	local flags = 0
	local veh = GetPlayerVehicle()
	if veh ~= 0 then flags = flags + 1 end
	if GetPlayerGrabBody() ~= 0 then flags = flags + 2 end
	if tdvrOffFarHold() then flags = flags + 32 end   -- (main libre : objet tenu de loin, voir offHand.keep)
	local mapView = GetBool("game.map.enabled")   -- carte du niveau (en 3D : caméra gérée par la DLL)
	if mapView then flags = flags + 4 end
	-- Si un autre mod impose la caméra (SetCameraTransform) tout près des yeux du
	-- joueur (mod de caméra, lunette de visée...), la DLL garde la vue normale de
	-- la tête. Plus loin (vue de l'avion de l'AC130, drone, cinématique), elle
	-- montre la caméra du mod en VR : on regarde autour de soi depuis elle.
	-- (caméra de l'image précédente comparée à celle du joueur ; 1,5 m pour entrer,
	-- 2 m pour sortir : pas de va-et-vient autour de la limite)
	-- (GetCameraTransform(1) : la vraie caméra, voir updateAim)
	local camGap = VecLength(VecSub(GetCameraTransform(1).pos, GetPlayerCameraTransform().pos))
	modCamNear = camGap < (modCamNear and 2.0 or 1.5) * math.max(1, lastScale)
	if modCamNear then flags = flags + 16 end

	local st = request(flags)
	if st == nil then
		if wasActive then onDeactivate() end
		return
	end
	if st.paused then return end
	if not wasActive then onActivate() end
	if st.recenter ~= lastRecenter then
		lastRecenter = st.recenter
		vrYaw = GetPlayerYaw()
		roomOffset = Vec(0, 0, 0)
	end

	-- Image gelée (œil droit de la stéréo synchronisée) : le monde ne bouge pas,
	-- on ne fait que replacer la caméra, l'outil et le laser.
	local frozen = st.frozen
	frozenFrame = frozen
	local debugLua = st.debugFlags % 2 == 1
	camDebug = debugLua
	noAimDebug = math.floor(st.debugFlags / 8) % 2 == 1
	yawWriter = math.floor(st.debugFlags / 16) % 2 == 1
	-- (aussi sur la carte en 3D : la vue tourne autour du point regardé)
	-- (sur la carte, joystick droit vers le haut / bas = zoom : il ne tourne pas en même temps)
	local tx, ty = st.rsx, st.rsy
	if st.leftHanded == 1 then tx, ty = st.lsx, st.lsy end
	if not frozen and not (mapView and math.abs(ty) > math.abs(tx)) then updateTurning(st, dt) end

	-- Caméra du joueur avant toute modification de cette image (voir plus bas).
	local pc0 = GetPlayerCameraTransform()
	local pt0 = GetPlayerTransform()
	local aim0 = QuatEuler(GetPlayerPitch(), GetPlayerYaw(), 0)
	aimSetThisTick = false

	local anchorRot = QuatEuler(0, vrYaw, 0)
	local scale = st.worldScale
	local origin
	local move = nil
	local active = st.uiOpen == 0 and not mapView
	local toolTarget = nil

	if veh ~= 0 then
		-- En véhicule : la "pièce" VR est attachée au siège et tourne avec le véhicule
		-- (le joystick droit tourne toujours la vue par rapport à lui).
		local eye = GetPlayerEyeTransform()
		local vt = GetVehicleTransform(veh)
		local vy = yawOf(vt.rot)
		local vf = QuatRotateVec(vt.rot, Vec(0, 0, -1))
		if lastVehYaw == nil then
			-- en montant : la tête regarde l'avant du véhicule
			if not frozen then vrYaw = vy - yawOf(st.head.rot) end
		elseif vf[1] * vf[1] + vf[3] * vf[3] > 0.25 then
			-- (avant presque vertical, véhicule qui culbute : lacet sans valeur, ignoré)
			local d = vy - lastVehYaw
			if d > 180 then d = d - 360 elseif d < -180 then d = d + 360 end
			vrYaw = vrYaw + d
		end
		if vrYaw > 180 then vrYaw = vrYaw - 360 elseif vrYaw < -180 then vrYaw = vrYaw + 360 end
		if lastVehYaw or not frozen then lastVehYaw = vy end
		anchorRot = QuatEuler(0, vrYaw, 0)
		origin = VecSub(eye.pos, QuatRotateVec(anchorRot, Vec(0, st.refHeadY * scale, 0)))
	else
		lastVehYaw = nil
		if st.roomscale == 1 and active and not frozen then
			move = roomScaleStep(st, anchorRot, scale, pt0.pos)
		end
		-- La pièce VR est accrochée à la caméra du joueur (pc0), pas à ses pieds ni
		-- à GetPlayerEyeTransform : le décalage de caméra s'applique à la caméra du
		-- joueur, et le jeu lisse sa hauteur (sauts, marches) à son propre rythme.
		-- Accrochés ailleurs, les deux yeux d'une paire se retrouvaient à des
		-- hauteurs différentes pendant un saut (mesuré : jusqu'à 9,6 cm).
		local base = move and VecAdd(pc0.pos, move) or pc0.pos
		local baseY
		if st.physicalHeight == 1 then
			baseY = base[2] - 1.7          -- caméra du jeu : 1,7 m au-dessus des pieds
		else
			baseY = base[2] - st.refHeadY * scale
		end
		origin = Vec(base[1], baseY + st.heightOffset, base[3])
	end

	-- (fastLua : chaque pose n'est calculée qu'une fois par image - têtes, mains, œil : 10 à 12 appels
	-- sinon ; ni l'entrée ni le résultat ne sont jamais modifiés en place, et rien de ce dont toWorld
	-- dépend ne change après ce point de tick)
	local twMemo = st.fast and {} or nil
	local function toWorld(tr)
		local w = twMemo and twMemo[tr]
		if w then return w end
		local rp = Vec(tr.pos[1] - roomOffset[1], tr.pos[2], tr.pos[3] - roomOffset[3])
		local p = VecAdd(origin, QuatRotateVec(anchorRot, VecScale(rp, scale)))
		w = Transform(p, QuatRotateQuat(anchorRot, tr.rot))
		if twMemo then twMemo[tr] = w end
		return w
	end

	local cam = toWorld(st.eyePose)
	RequestFirstPerson(false) -- à chaque tick : la demande ne vaut que pour une image
	-- Lacet de la pièce VR dans le monde : la DLL construit la rotation de la vue
	-- avec ta tête seule (la caméra du jeu suit la visée et tremblait avec le bras).
	-- (pas utile pour l'œil droit : la DLL le rend depuis la caméra de l'œil gauche)
	if not frozen and math.floor(st.debugFlags / 2) % 4 == 0 then
		pcall(SetClipboardText, string.format("TDVRA %.3f", yawOf(anchorRot)))
	end

	if veh == 0 and active then
		if not frozen then updateLocomotion(st, toWorld, dt) end
		updateAim(st, toWorld, cam, frozen, move)
		local feet = move and VecAdd(pt0.pos, move) or pt0.pos
		local ptRot = aimSetThisTick and QuatEuler(0, lastAimYaw, 0) or pt0.rot
		toolTarget = updateTool(st, toWorld, Transform(VecAdd(feet, VecScale(GetPlayerUp(), 1.7)), ptRot), frozen, dt)
	end

	-- La caméra du joueur reste en mode normal (sinon le jeu cache l'outil en
	-- main) : on la décale jusqu'à l'œil et on impose le champ de vision.
	-- Le décalage s'applique à la caméra du joueur telle qu'elle sera rendue,
	-- donc après la visée et le déplacement roomscale de cette image.
	local pcRot = pc0.rot
	if aimSetThisTick then
		local q = aim0
		pcRot = QuatRotateQuat(QuatRotateQuat(QuatEuler(lastAimPitch, lastAimYaw, 0), Quat(-q[1], -q[2], -q[3], q[4])), pc0.rot)
	end
	local pc = Transform(move and VecAdd(pc0.pos, move) or pc0.pos, pcRot)
	if not mapView then
		SetCameraOffsetTransform(TransformToLocalTransform(pc, cam))
		SetCameraFov(st.fovH)
	end

	-- Test du HUD (debugFovTest) : repère jaune à 50 m dans l'axe de la caméra du
	-- test du champ de vision (plus bas : horizontale, elle ne suit que le lacet de
	-- la pièce, pas la tête) ; le viseur 2D vert dessiné au centre du canevas (draw)
	-- doit le recouvrir dans les deux yeux (sinon le HUD est vu dédoublé).
	hudTest.fov = math.floor(st.debugFlags / 4) % 2 == 1
	if hudTest.fov then
		local hrot = QuatEuler(0, vrYaw, 0)
		local hf = QuatRotateVec(hrot, Vec(0, 0, -1))
		local hx = QuatRotateVec(hrot, Vec(1, 0, 0))
		local hc50 = VecAdd(toWorld(st.head).pos, VecScale(hf, 50))
		DrawLine(VecAdd(hc50, Vec(0, -2, 0)), VecAdd(hc50, Vec(0, 2, 0)), 1, 1, 0)
		DrawLine(VecAdd(hc50, VecScale(hx, -2)), VecAdd(hc50, VecScale(hx, 2)), 1, 1, 0)
	end
	-- Casque simulé seulement : clic court du joystick gauche = une caméra imposée
	-- comme celle de l'AC130 (40 m au-dessus, 30 m derrière, visant 20 m devant le
	-- joueur) avec une croix jaune 3D sur la cible : le viseur 2D vert (draw) doit
	-- la recouvrir dans les deux yeux, même en tournant la tête.
	if math.floor(st.debugFlags / 32) % 2 == 1 then
		local click = math.floor(st.buttons / 16) % 2 == 1
		if not frozen then
			if click and not hudTest.prevClick then hudTest.modCam = not hudTest.modCam end
			hudTest.prevClick = click
		end
	else
		hudTest.modCam = false
	end
	if hudTest.modCam and not mapView then
		local fwd = QuatRotateVec(anchorRot, Vec(0, 0, -1))
		local target = VecAdd(pt0.pos, VecScale(fwd, 20))
		local from = VecAdd(VecAdd(pt0.pos, VecScale(fwd, -30)), Vec(0, 40, 0))
		SetCameraTransform(Transform(from, QuatLookAt(from, target)), 60)
		DrawLine(VecAdd(target, Vec(-1.5, 0, 0)), VecAdd(target, Vec(1.5, 0, 0)), 1, 1, 0)
		DrawLine(VecAdd(target, Vec(0, 0, -1.5)), VecAdd(target, Vec(0, 0, 1.5)), 1, 1, 0)
		DrawLine(target, VecAdd(target, Vec(0, 3, 0)), 1, 1, 0)
	end
	syncTest = math.floor(st.debugFlags / 2) % 2 == 1
	if math.floor(st.debugFlags / 4) % 2 == 1 then
		-- Test du champ de vision : caméra horizontale, ligne verte verticale à
		-- 30° à droite, ligne magenta horizontale à 20° au-dessus, à 5 m.
		local rot = QuatEuler(0, vrYaw, 0)
		SetCameraOffsetTransform(TransformToLocalTransform(pc, Transform(cam.pos, rot)))
		local function dirAt(yawRight, pitchUp)
			return QuatRotateVec(QuatRotateQuat(rot, QuatEuler(pitchUp, -yawRight, 0)), Vec(0, 0, -1))
		end
		local c = VecAdd(cam.pos, VecScale(dirAt(30, 0), 5))
		DrawLine(VecAdd(c, Vec(0, -1, 0)), VecAdd(c, Vec(0, 1, 0)), 0, 1, 0)
		-- Repère fixe dans le monde (même pour les deux yeux) : ligne bleue à 3 m
		-- devant le centre de la tête, pour mesurer l'écart entre les yeux.
		local hc = toWorld(st.head).pos
		local c3 = VecAdd(hc, VecScale(QuatRotateVec(rot, Vec(0, 0, -1)), 3))
		DrawLine(VecAdd(c3, Vec(0, -0.5, 0)), VecAdd(c3, Vec(0, 0.5, 0)), 0, 0, 1)
		local c2 = VecAdd(cam.pos, VecScale(dirAt(0, 20), 5))
		local right = QuatRotateVec(rot, Vec(1, 0, 0))
		DrawLine(VecAdd(c2, VecScale(right, -1)), VecAdd(c2, VecScale(right, 1)), 1, 0, 1)
		if not fovTestLogged then
			fovTestLogged = true
			debugMsg = string.format("fovtest fovH=%.3f fovV=%.3f", st.fovH, st.fovV)
		end
	end
	if syncTest then
		-- Œil gauche vers le ciel, œil droit vers le sol (voir SyncTest dans la DLL).
		syncEye = st.eye
		SetCameraTransform(Transform(cam.pos, QuatEuler(st.eye == 0 and 80 or -80, vrYaw, 0)), 90)
	end

	if not frozen then updateHaptics(st) end
	tdvrOffArmTick(st, toWorld, frozen, veh == 0 and not mapView and modCamNear)   -- bras de la main libre
	pcall(tdvrDualTick, st, toWorld, frozen, veh == 0 and not mapView and modCamNear)   -- armes doubles
	updateInventory(st, dt)   -- inventaire VR (images normales seulement)
	if not frozen then mockPushTest(st, dt) end
	if not frozen then
		toolPushedBody = 0
		if veh == 0 and active then updateSledge(st, dt, toolTarget) else sledgePrev = nil end
		if not (veh == 0 and active and st.aimMode ~= 1) then
			colPrev = nil
			if phys.body ~= 0 then physDestroy() end
		end
	end
	if sledgeHapT > 0 then
		sledgeHapT = sledgeHapT - (frozen and 0 or dt)
		if st.leftHanded == 1 then hapL = math.max(hapL, sledgeHapA) else hapR = math.max(hapR, sledgeHapA) end
	end
	if not frozen then updateFog(st, dt) end
	if not frozen then updateBloom(st, dt) end
	-- Profondeur de champ : coupée à chaque image (comme le fait le HUD du jeu).
	if st.removeDof then pcall(SetCameraDof, 0, 0) end
	lastCam = cam
	lastCamEye = st.eye
	lastEyeRoom = st.eyePose
	lastScale = st.worldScale

	if st.aimMode == 0 then SetBool("hud.aimdot", false) end
	if st.hideHud == 1 then SetBool("hud.disable", true) end
	if st.vignette == 0 then vignette = 0 end

	if debugInfo then
		DebugWatch("VR eye", st.eye)
		DebugWatch("VR fov", string.format("%.1f x %.1f", st.fovH, st.fovV))
		DebugWatch("VR yaw", vrYaw)
		DebugWatch("player yaw", GetPlayerYaw())
	end
end

-- Diagnostic : cohérence de la paire stéréo. La caméra réellement rendue pour
-- l'œil droit, vue depuis celle de l'œil gauche, doit être exactement l'écart
-- voulu entre les deux yeux (sinon : désynchronisation, fatigue visuelle).
local stereoA, stereoAWanted = nil, nil
local stereoErr = { 0, 0, 0, 0, 0, 0 } -- max écart position, max angle, nb paires fausses, x, y, z de l'écart max
local stereoPairs = 0

local function angleBetween(qa, qb)
	local fa = QuatRotateVec(qa, Vec(0, 0, -1))
	local fb = QuatRotateVec(qb, Vec(0, 0, -1))
	local ua = QuatRotateVec(qa, Vec(0, 1, 0))
	local ub = QuatRotateVec(qb, Vec(0, 1, 0))
	return math.max(math.deg(math.acos(clamp(VecDot(fa, fb), -1, 1))), math.deg(math.acos(clamp(VecDot(ua, ub), -1, 1))))
end

local function checkStereo(real)
	if lastCamEye == 0 then
		stereoA, stereoAWanted = real, lastEyeRoom
		return
	end
	if not stereoA or not stereoAWanted then return end
	-- Écart voulu : celui des deux yeux dans le casque (même instant), à l'échelle du monde.
	local relReal = TransformToLocalTransform(stereoA, real)
	local relRoom = TransformToLocalTransform(stereoAWanted, lastEyeRoom)
	local relWanted = Transform(VecScale(relRoom.pos, lastScale), relRoom.rot)
	local d = VecSub(relReal.pos, relWanted.pos)
	local dp = VecLength(d)
	local da = angleBetween(relReal.rot, relWanted.rot)
	if dp > stereoErr[1] then stereoErr[1] = dp; stereoErr[4], stereoErr[5], stereoErr[6] = d[1], d[2], d[3] end
	stereoErr[2] = math.max(stereoErr[2], da)
	if dp > 0.003 or da > 0.15 then stereoErr[3] = stereoErr[3] + 1 end
	stereoPairs = stereoPairs + 1
	stereoA = nil
end

-- Diagnostic : la caméra rendue cette image est-elle celle demandée par tick() ?
local function checkCamera()
	if not camDebug or not lastCam then return end
	local real = GetCameraTransform(1)   -- la vraie caméra rendue (voir updateAim)
	checkStereo(real)
	local dp = VecLength(VecSub(real.pos, lastCam.pos))
	local fa = QuatRotateVec(real.rot, Vec(0, 0, -1))
	local fb = QuatRotateVec(lastCam.rot, Vec(0, 0, -1))
	local da = math.deg(math.acos(clamp(VecDot(fa, fb), -1, 1)))
	local k = lastCamEye == 1 and 3 or 0
	camErr[k + 1] = math.max(camErr[k + 1], dp)
	camErr[k + 2] = math.max(camErr[k + 2], da)
	if dp > 0.01 or da > 0.5 then
		camErr[k + 3] = camErr[k + 3] + 1
		if camBadLogs < 12 then
			camBadLogs = camBadLogs + 1
			local pc = GetPlayerCameraTransform()
			local fp = QuatRotateVec(pc.rot, Vec(0, 0, -1))
			local dpp = VecLength(VecSub(real.pos, pc.pos))
			local dpa = math.deg(math.acos(clamp(VecDot(fa, fp), -1, 1)))
			camBadDetail = string.format("badcam eye=%d dp=%.3f da=%.2f vsPlayerCam dp=%.3f da=%.2f tickdt=%.4f ticks=%d draws=%d",
				lastCamEye, dp, da, dpp, dpa, lastTickDt, tickCount, drawCount)
		end
	end
	camErrN = camErrN + 1
	if camErrN >= 240 then
		local pp = GetPlayerTransform().pos
		debugMsg = string.format("camerr left pos=%.4f ang=%.3f bad=%d | right pos=%.4f ang=%.3f bad=%d | room=%.2f,%.2f player=%.2f,%.2f",
			camErr[1], camErr[2], camErr[3], camErr[4], camErr[5], camErr[6], roomOffset[1], roomOffset[3], pp[1], pp[3])
		camErr = { 0, 0, 0, 0, 0, 0 }
		camErrN = 0
		aimMsg = string.format("aimcheck yaw=%.2f want=%.2f pitch=%.2f want=%.2f", GetPlayerYaw(), lastAimYaw or 0,
			GetPlayerPitch(), lastAimPitch or 0)
		stereoMsg = string.format("stereo pairs=%d bad=%d maxpos=%.4f (x=%.4f y=%.4f z=%.4f, repère œil gauche) maxang=%.3f",
			stereoPairs, stereoErr[3], stereoErr[1], stereoErr[4], stereoErr[5], stereoErr[6], stereoErr[2])
		stereoErr = { 0, 0, 0, 0, 0, 0 }
		stereoPairs = 0
	end
end

function draw()
	drawCount = drawCount + 1
	-- Image dessinée sans tick depuis la précédente : l'œil droit quand la DLL ne refait pas la mise
	-- à jour de l'image B (frameBMode=2). Les mesures de checkCamera (caméra, œil, pose voulue)
	-- viennent du tick de l'œil gauche : elles ne valent pas pour elle.
	if tickCount ~= drawTick then checkCamera() end
	drawTick = tickCount
	if (hudTest.fov or hudTest.modCam) and wasActive then
		-- Viseur 2D de test au centre du canevas (voir tick).
		UiPush()
		UiTranslate(UiCenter(), UiMiddle())
		UiAlign("center middle")
		UiColor(0, 1, 0, 1)
		UiRect(80, 4)
		UiRect(4, 80)
		UiPop()
	end
	if syncTest and wasActive then
		-- Repère de synchronisation : rouge = œil gauche, bleu = œil droit.
		UiPush()
		UiColor(syncEye == 0 and 1 or 0, 0, syncEye == 1 and 1 or 0, 1)
		UiRect(24, 24)
		UiPop()
	end
end
