// Screens. Layout follows the instrument's own menu structure (owner's
// manual: Piano / Sound / Music tabs, Metronome, Virtual Technician, Settings) so panel users find things
// where they expect them — except that the Piano tab is folded into the Sound tab (user 2026-11-09: one place
// shows a sound and everything choosing it changes), and that recording, the piano's own songs and the USB memory
// are one Recorder tab (ui/recorder.ts, owner 2026-11-22) while the Music tab keeps Lesson / Concert Magic / Piano Music.
// Every visible string goes through i18n: t() for UI text, tl() for labels
// that come from the protocol tables.
// One module per tab under ui/screens/; this file is the entry the app and the tests import.
export { TAB_TITLES } from "./screens/common.ts";
export { metronomeScreen } from "./screens/metronome.ts";
export { CM_RESULT, MUSIC_MODE, musicScreen, songSelectData } from "./screens/music.ts";
export { settingsScreen } from "./screens/settings.ts";
export { soundScreen } from "./screens/sound.ts";
export { PREVIEW_INDEX, vtScreen } from "./screens/technician.ts";
export { valueLabel } from "./controls.ts";
