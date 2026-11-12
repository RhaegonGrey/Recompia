// Text the Rust side (src-tauri) sends to the page, in the player's language: error sentences, the readiness detail, preflight checks, setup and
// play progress. Rust writes English only, so each sentence is matched by its English text and translated through i18n.js.
//
//   TEXTS     one entry per sentence Rust can send, in Rust's own words. {1}, {2}... stand for the `{}` of its format! string (paths, another
//             message); {n1}, {n2}... for a number (a count, a size), which matches digits only; a text with none is matched whole. Keep an entry equal to the Rust string it stands for: a changed
//             sentence stops matching and shows in English until its entry follows.
//   fromBackend(text)  the text in the player's language: the whole text when it matches an entry, else each of its sentences (Rust joins
//             several into one message), else the text as it is. What a {n} holds is translated too, since it may be another message.
//
// Pure, no DOM and no Tauri, so `node --test` runs it. Pages call fromBackend() where they show a backend text; logic that reads a backend
// text (action.js gamePhase) keeps the raw one.
import { msg, t } from './i18n.js';

const TEXTS = [
  // backup_cmds.rs, profile_cmds.rs, repair_cmds.rs, saves.rs, update_cmds.rs, guard.rs, dump_cmds.rs: refusals while the game or a job runs
  msg('The game is running. Close it first, then restore a backup.'),
  msg('The game is running. Close it first, then change profiles.'),
  msg('The game is running. Close it first, then repair.'),
  msg('The game is running. Close it first, then import the save.'),
  msg('The game is running. Close it first, then update.'),
  msg('Setup is running. Wait for it to finish, then repair.'),
  msg('Setup is running. Pause it first, then update.'),
  msg('Setup is running. Pause it first.'),
  msg('An update is being installed. Wait for it to finish.'),
  msg('The shader files are being repacked. Wait for it to finish, or stop it in Settings.'),
  msg('The launcher settings folder is not known.'),
  msg('That link cannot be opened from here.'),
  msg('Could not open {1}: {2}'),

  // dlss.rs
  msg('This package has no DLSS download tool. Extract the release zip again so tools/ is complete.'),
  msg('The download needs the Linux build tools that Set up fetches: run Set up first.'),
  msg('The DLSS download failed ({1}). {2}'),

  // dump.rs: the game files check
  msg('copied in text mode'),
  msg('romfs/ is complete'),
  msg('romfs/ is not empty'),
  msg('romfs/ holds the game data'),
  msg('romfs/ completed from {1}'),
  msg('{1} ({2} of {3} files, {4} of {5} GB)'),
  msg('exefs/{1} {2} ({3} bytes short)'),
  msg('older version'),
  msg('version {1}'),
  msg('No folder chosen yet.'),
  msg('That folder does not exist or cannot be read.'),
  msg('This folder has no base/ or updates/ subfolder, and no exefs/ and romfs/ either. Pick the folder that contains them (for example Pokopia_v2.0.1a).'),
  msg('Some game files are shorter than they should be: the dump was copied in text mode, which damages them. Copy the exefs and romfs folders again in binary mode (FileZilla: Transfer > Transfer type > Binary).'),
  msg('Update the game on your Switch 2 to {1}, then dump again.'),
  msg('These game files are from an older version. {1}'),
  msg('This dump holds the base game only. {1}'),
  msg('These game files are a different version. This release needs {1}.'),
  msg('The romfs folder is empty or incomplete. Dump the game again, or copy the romfs folder again in binary mode.'),
  msg('No usable game files found. Each of base/ or an updates/<version>/ folder needs exefs/ (main, sdk, subsdk0, rtld, main.npdm) and romfs/.'),
  msg('Choose your game files first.'),
  msg('Choose usable game files first.'),
  msg('The game files at {1} are not usable: {2}'),

  // events.rs
  msg("Unknown timed event '{1}' in pkp.ini. Choose a valid event or turn all event toggles off."),
  msg("The local start time for timed event '{1}' is not available in this time zone."),

  // gamebuild.rs, readiness.rs, setup_job.rs
  msg('This update changes the game code. Press Finish update to rebuild it (about 7 minutes). Your shaders and saves are kept.'),
  msg('This update changes the runtime only. Press Finish update to link the game again (about a minute). Your shaders and saves are kept.'),
  msg('pkp_rt.dll next to the launcher is from another release than the build tools in the tools folder ({1} and {2}), so Set up would build a pkp.exe that does not run with it. Only part of an update is in place: extract the whole release zip over this folder, every file, then press Set up again.'),
  msg('The game build cannot run, missing: {1}. Extract the release zip again so tools/ is complete.'),
  msg('The game build failed ({1}). {2}'),
  msg('pkp.exe was not found in {1}. Put the launcher next to pkp.exe.'),
  msg('pkp_rt.dll was not found next to pkp.exe in {1} nor next to the launcher. Extract the release again so the launcher\'s folder has it, or set the game folder in Settings.'),
  msg('Setup has not finished. Press Continue setup: it picks up where it stopped.'),
  msg('Your mods need new hook points: Set up again (a few minutes; shaders and saves are kept).'),

  // mods_cmds.rs, mods.rs, mods_catalog.rs, mods_install.rs, download.rs: the Mods page
  msg('The game is running. Close it first, then change your mods.'),
  msg('Setup is running. Wait for it to finish, then change your mods.'),
  msg('There is no mod {1} in the mods folder.'),
  msg("Could not save the mods' list in the mods folder."),
  msg("This mod's code is not allowed yet."),
  msg('Community Achievements is locked: unlock it first to switch it off or remove it.'),
  msg('Sharing of Community Achievements is locked: unlock it first to switch it off.'),
  msg('Could not record your yes for {1}.'),
  msg('Could not change {1} of {2}.'),
  msg('Could not delete {1}: {2}'),
  msg('The mod list cannot be read: {1}'),
  msg('The mod list is in a newer format than this launcher reads. Update the launcher to browse mods.'),
  msg('The mod list could not be loaded. Check the internet connection and try again.'),
  msg('Installing needs the mod hub, which cannot be reached now. Check the internet connection and try again.'),
  msg('The mod hub is not open for installs yet.'),
  msg('{1} is not in the mod list for this game.'),
  msg('{1} is not out yet.'),
  msg("{1} cannot be installed: its download is not on the mod hub's server."),
  msg('{1} does not match its published size and SHA-256 ({n1} bytes, {2}): not kept.'),
  msg('Could not download {1}: {2}'),
  msg('Cannot create {1}: {2}'),
  msg('Press Set up to build the game and prepare its shaders.'),
  msg('This version fixes how some shaders are translated. Press Finish update: it translates only those shaders again.'),
  msg('Your update holds only the files it changes, so the launcher now adds the missing ones from the base game. Press Finish update so their shaders are prepared too.'),
  msg('pkp.exe was not found in {1}. Put the launcher next to pkp.exe or set the game folder in Settings.'),
  msg('Setup is already running.'),
  msg('Checking the setup tools...'),
  msg('Running the game once in the background to collect the shaders it builds while it runs (about {n1} s)...'),
  msg('Setup paused. Set up again continues where it stopped.'),
  msg('There is no setup log yet: it is written when Set up starts.'),

  // host.rs: the Linux build tools
  msg('Nothing to run the game\'s Windows programs with was found. Install Steam and its Proton (in Steam: Library, Tools, Proton Experimental), umu-launcher, or Wine.'),
  msg('Downloading the Linux build tools: {n1} of {n2} MB'),
  msg('Installing the Linux build tools: Python...'),
  msg('Installing the Linux build tools: Zig...'),

  // lan.rs
  msg('Windows treats this network as Public, so other PCs may not reach you. Set it to Private in Settings > Network.'),
  msg('If Windows Firewall asks about this program, allow it on Private networks. If it was answered wrongly once, remove the Block rules for the program in Windows Defender Firewall > Allow an app.'),
  msg('If visitors cannot connect, the firewall may block the ports (47600 for a host, 47610 for a relay). With firewalld (Fedora, Bazzite): sudo firewall-cmd --add-port=47600/tcp --add-port=47600/udp --add-port=47610/tcp --add-port=47610/udp'),

  // nsp.rs, nup.rs: 'Cannot write {1} (disk full or blocked?): {2}' comes first, the plainer 'Cannot write {1}: {2}' would match it too
  msg('Cannot write {1} (disk full or blocked?): {2}'),
  msg('The .nsp file is damaged or incomplete.'),
  msg('This is not an .nsp file (no PFS0 header).'),
  msg('This .nsp is still encrypted. Use decrypted NCA files, as for the game.'),
  msg('Add-on {1} is not for this game.'),
  msg('This .nsp holds no add-on content for this game.'),

  // nup.rs
  msg('Unpacking was cancelled.'),
  msg('The .nup file is damaged or incomplete.'),
  msg('This is not a .nup package (wrong header).'),
  msg('This .nup was made with a newer format than this launcher knows. Update the launcher.'),
  msg('The .nup file is damaged or incomplete (it does not match its header). Download it again.'),
  msg('The .nup table of contents is damaged (checksum mismatch). Download the file again.'),
  msg('This .nup has no base game or update in it.'),
  msg('{1} already exists and was not made by the launcher. Move or delete it, then try again.'),
  msg('This .nup is not the game version this launcher supports (build {1}). It holds: {2}.'),
  msg('This update only holds the files it changes and needs the base game, which is not in this .nup. Use a .nup that contains the base game too.'),
  msg('The game program in this .nup has no RomFS (game data).'),
  msg('Not enough free disk space to unpack the game: about {1} GB are needed on the drive of {2}, {3} GB are free.'),

  // play.rs: starting and watching the game (the raw text is what action.js gamePhase reads)
  msg('The game is already running.'),
  msg('Collecting the shaders the game builds while it runs (once, about a minute)...'),
  msg('Checking the shaders the game builds while it runs...'),
  msg('{n1} shaders were translated before the start.'),
  msg('Shaders were not prepared: {1}.'),
  msg('Could not start pkp.exe: {1}'),
  msg('pkp.exe started (process {1})'),
  msg('Translating the shaders the game built while it ran...'),
  msg('{n1} new shaders translated.'),
  msg('New shaders were not translated: {1}.'),
  msg('Lost track of pkp.exe: {1}'),
  msg('pkp.exe exited normally.'),
  msg('pkp.exe exited with code {1} (0x{2}).'),
  msg('pkp.exe was ended by the system.'),
  msg('pkp.exe in {1} is missing or from an older release: open the launcher and press Set up.'),
  msg('Setup has not finished: open the launcher and press Set up.'),
  msg('The profile could not be applied, so the game was not started: {1}'),

  // preflight.rs: the checks before Set up
  msg('Not enough free disk space'),
  msg('{1} has {2} free and setup needs about {3} (the game build, then about 5 GB of shader sources that are deleted at the end and about {4} of translated shaders that stay). Free some space, or move the game folder to a bigger drive (Settings, game folder).'),
  msg('7 GB (packed)'),
  msg('Free disk space'),
  msg('{1} free on {2}; setup needs about {3}.'),
  msg('Free disk space unknown'),
  msg('Could not read the free space of {1}. Setup needs about {2}.'),
  msg('Game folder'),
  msg('Setup can write in {1}.'),
  msg('Vulkan not found'),
  msg('{1} did not load ({2}). It comes with the graphics driver: install the current driver from NVIDIA, AMD or Intel. Setup can finish without it, but the game cannot start.'),
  msg('No Vulkan GPU found'),
  msg('{1}. Update the graphics driver. Setup can finish without it, but the game cannot start.'),
  msg('Vulkan only offers a software renderer. Update the graphics driver. Setup can finish without it, but the game cannot start.'),
  msg('Vulkan 1.3 needed'),
  msg('{1}. The game needs Vulkan 1.3: update the graphics driver.'),
  msg('Vulkan'),
  msg('Proton'),
  msg('The shader translator and the game run with {1}.'),
  msg('No Proton found'),
  msg('Less than 16 GB of RAM'),
  msg('This PC has {1}. The game wants 16 GB. Setup still works, but the build runs fewer compiles at once and takes longer.'),
  msg('Memory'),
  msg('{1} of RAM.'),
  msg('Memory unknown'),
  msg('Could not read the amount of RAM.'),
  msg('Processor'),
  msg('{1} runs microcode {2}, which has Intel\'s fix.'),
  msg('Intel 13th/14th generation CPU: update the BIOS'),
  msg('{1} runs microcode {2}. These CPUs can become unstable, and crash Windows under long heavy loads like the shader translation (1 to 2 hours on every core), until the motherboard\'s BIOS carries Intel\'s microcode fix (0x12B or newer). Update the BIOS from your motherboard maker first, and turn on "Use fewer cores" below.'),
  msg('{1} has microcode that could not be read. These CPUs can become unstable, and crash Windows under long heavy loads like the shader translation (1 to 2 hours on every core), until the motherboard\'s BIOS carries Intel\'s microcode fix (0x12B or newer). Update the BIOS from your motherboard maker first, and turn on "Use fewer cores" below.'),

  // profiles.rs
  msg('profiles.json is damaged and could not be copied aside ({1}), so nothing was changed.'),
  msg('Type a name for the profile.'),
  msg('The name can have at most {n1} characters.'),
  msg('The name cannot have control characters.'),
  msg('There is already a profile called {1}.'),
  msg('That profile does not exist.'),
  msg('The first profile holds the existing save and cannot be removed.'),

  // relay_cmds.rs
  msg('Cannot open port {1}: {2}'),
  msg('Cannot make a room code: {1}'),
  msg('A room code has 16 letters and digits: XXXX-XXXX-XXXX-XXXX.'),

  // repair.rs, rollback.rs
  msg('Pick at least one group of game settings to reset.'),
  msg('Your saves and settings could not be copied first ({1}), so nothing was changed.'),

  // savebackup.rs
  msg('Cannot finish the backup: {1}'),
  msg('The save was being written. Nothing was copied; try again in a moment.'),
  msg('That is not a backup name.'),
  msg('That backup does not exist (any more).'),
  msg("That backup belongs to another profile's save."),
  msg('That backup is incomplete.'),
  msg('Your current progress could not be saved first, so nothing was changed: {1}'),
  msg('Cannot copy the backup: {1}. Your save was not changed.'),
  msg('The copy of the backup is not complete. Your save was not changed.'),
  msg('Cannot replace the save: {1}. Your save was not changed.'),
  msg('Cannot put the backup in place: {1}. Your save was not changed.'),

  // saveimport.rs, saves.rs
  msg('{1} holds no Pokopia save (no G00 and S00 folders).'),
  msg('{1} holds no Pokopia save (it needs the G00 and S00 folders).'),
  msg('{1} is inside the save folder the game plays. Pick a copy outside it.'),
  msg('Copying the save failed ({1}). Your current save was not touched.'),
  msg('The folder holds no save files.'),
  msg('Could not move the old save aside ({1}). Nothing was changed.'),
  msg('Could not put the imported save in place ({1}). The old save was put back.'),
  msg('The save was copied to {1} but its .meta could not be written. Start the game once to let it create one.'),
  msg('No Pokopia save found in Eden or Ryujinx. Use "Pick a folder" instead.'),

  // settings.rs
  msg('{1} does not exist yet. Start the game once so it creates it.'),

  // shaders.rs, storage.rs: shader work
  msg('Finding the game\'s shaders in your game files...'),
  msg('Scanning game files: {n1} of {n2}'),
  msg('Checking the shaders an earlier version translated...'),
  msg('Checking the shaders an earlier version translated: {n1} of {n2}'),
  msg('Translating shaders: {n1} of {n2}'),
  msg('Shader setup cannot run, missing: {1}. Re-extract the launcher\'s release folder so tools/ is complete.'),
  msg('Setup is running. Wait for it to finish, or pause it first.'),
  msg('The game is running. Close it first: it reads the shader files while it runs.'),
  msg('The shader files are already being repacked.'),
  msg('Loose shader files need about {1} GB more free space on {2} ({3} GB free).'),

  // steam.rs
  msg("Steam's shortcut list could not be read ({1}). Nothing was changed."),
  msg('Close Steam first (Steam, Exit), then press the button again. Steam only saves its shortcut list when it closes.'),
  msg('Steam was not found on this PC, or nobody has signed in to it yet. Start Steam once, sign in, close it, and try again.'),
  msg('Added "{1}" to Steam for 1 account. Start Steam again to see it, with its artwork.'),
  msg('Added "{1}" to Steam for {2} accounts. Start Steam again to see it, with its artwork.'),   // Rust adds the plural s: one entry per form
  msg('Close Steam first (Steam, Exit), then press the button again.'),
  msg('It was not in Steam.'),
  msg('Removed "{1}" from Steam. Start Steam again.'),

  // update.rs, update_cmds.rs
  msg('Profiles (a separate save and name for each player) arrived in 0.8.0. An older version ignores profiles.json and uses one save, so a second player\'s save may not be found.'),
  msg('The one-click updater arrived in 0.7.5. An older launcher cannot update itself, so moving forward again would mean extracting a release by hand.'),
  msg('That version could not be compared with the one you have.'),
  msg('The game files may have to be built again for the older version, so Set up again can be asked (about 15 to 20 minutes).'),
  msg('Files that only the newer version has stay in the folder; they are not removed.'),
  msg('You already have that build.'),
  msg('Going back to an older build needs your confirmation first. Nothing was changed.'),
  msg('The download failed: {1}. Nothing was changed. Try again.'),
  msg('Could not save the update: {1}. Nothing was changed.'),
  msg('Could not unpack the update: {1}. Nothing was changed.'),
  msg("Could not start the system's tar to unpack the update: {1}. Nothing was changed."),
  msg('Could not unpack the update ({1}). Check that the drive has free space, then try again. Nothing was changed.'),
  msg('The downloaded package is not laid out as expected. Nothing was changed.'),
  msg('{n1} files could not be put back (first: {2}). Close the launcher and start it again; if this stays, extract the release again.'),
  msg('Could not read the unpacked update: {1}. Nothing was changed.'),
  msg('{1} is a folder in your game folder, but the update has a file of that name. Nothing was changed.'),
  msg('Could not write in the update folder: {1}. Nothing was changed.'),
  msg('The update could not be put in place: {1}. Everything was restored.'),
  msg('The update could not be put in place: {1}. {2}'),
  msg('Another launcher window is updating. Close it first.'),
  msg('An earlier update did not finish and could not be undone. Extract the release again.'),
  msg('The launcher cannot write in {1}: {2}. Nothing was changed.'),
  msg('Not enough free disk space on {1}: the update needs about {2} and {3} is free.'),
  msg('This release has no checksum for its download yet. Try again in a while. Nothing was changed.'),
  msg('The download arrived damaged. Nothing was changed. Try again.'),
  msg('An update is already running.'),
  msg('This build does not update itself.'),
  msg('No update is available right now.'),
  msg('That build cannot be installed on this system.'),
  msg("Could not find the launcher's own file."),
  msg('The new launcher did not start ({1}). The update was undone.'),
  msg('The new launcher did not start ({1}). {2}'),

  // writecheck.rs: can the launcher write in the game folder
  msg('Windows is blocking writes in the game folder'),
  msg("The game folder's drive is write-protected"),
  msg("The game folder's drive is full"),
  msg('Cannot write in the game folder'),
  msg('Windows is blocking writes in this folder ({1}). This is most likely Windows Security\'s Controlled folder access or an antivirus, which block programs in Documents, Desktop, Downloads and Pictures. Allow PokopiaLauncher.exe, pkp.exe and the tools it runs (python.exe, zig.exe and spirv_harness.exe in the tools folder) in Windows Security > Virus & threat protection > Ransomware protection > Allow an app through Controlled folder access, or in your antivirus. Or move the game folder somewhere like C:\\Games.'),
  msg('The drive is write-protected ({1}). Move the game folder to another drive, somewhere like C:\\Games.'),
  msg('The drive is full ({1}). Free some space, or move the game folder to a bigger drive.'),
  msg('Setup cannot write in this folder ({1}). Check that the folder is not read-only, or move the game folder somewhere like C:\\Games.'),
];

const escapeRe = (s) => s.replace(/[.*+?^$()|[\]\\{}]/g, '\\$&');

/** { text, re }: re matches the whole of a text built from the entry and captures what each {n} holds, in the order of the {n} in the entry. */
const ENTRIES = TEXTS.map((text) => {
  const order = [];
  const source = text.split(/(\{n?\d+\})/).map((part) => {
    const hole = /^\{(n?\d+)\}$/.exec(part);
    if (!hole) return escapeRe(part);
    order.push(hole[1]);
    return hole[1][0] === 'n' ? '(\\d[\\d.,]*)' : '(.+?)';   // {n1}: a number, so a count never swallows the sentence before it
  }).join('');
  return { text, order, re: new RegExp(`^${source}$`, 's') };
});

const SENTENCE_BREAK = /(?<=[.!?])\s+(?=[A-Z0-9"'(])/;

function whole(text) {
  for (const e of ENTRIES) {
    const m = e.re.exec(text);
    if (!m) continue;
    const params = {};
    e.order.forEach((n, i) => { params[n] = fromBackend(m[i + 1]); });
    return t(e.text, params);
  }
  return null;
}

/**
 * A text from the backend in the player's language. A text that is empty, already in another language or not one of TEXTS comes back as it
 * is (so a sentence Rust adds before TEXTS follows shows in English, never blank).
 */
export function fromBackend(text) {
  if (typeof text !== 'string' || !text) return text;
  const direct = whole(text);
  if (direct !== null) return direct;
  const sentences = text.trim().split(SENTENCE_BREAK);
  if (sentences.length < 2) return text;
  return sentences.map((s) => whole(s) ?? s).join(' ');
}
