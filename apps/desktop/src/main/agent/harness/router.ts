/**
 * Router: from the user's message to an intent set (code first, one cheap constrained model call
 * only when no rule fires) and from intents to a small toolset of 3-8 facade tools.
 */

export const INTENTS = ['undo', 'redo', 'export', 'canvas', 'captions', 'title', 'pauses', 'fillers', 'retakes', 'music', 'beats', 'clips', 'assemble', 'question'] as const;
export type Intent = (typeof INTENTS)[number];

const RULES: [Intent, RegExp][] = [
  ['undo', /\bundo\b|\brevert\b|\broll ?back\b|\bput (it|that|them) back\b|\bgo back\b/i],
  ['redo', /\bredo\b/i],
  ['export', /\bexport\b|\brender (it|this|the video|out)\b|\bsave (it |this )?as (an? )?(mp4|video|file)\b/i],
  ['canvas', /\b(vertical|portrait|horizontal|landscape|square)\b|\b(9|16|4|1|21):(16|9|5|3|1)\b|\breels?\b|\btiktok\b|\bshorts\b|\baspect( ratio)?\b|\bfps\b|\bframe ?rate\b|\bcanvas\b/i],
  ['captions', /\bcaptions?\b|\bsubtitles?\b/i],
  ['title', /\btitles?\b|\bheading\b|\blower third\b|\btext (that )?(says|reading)\b/i],
  ['pauses', /\bpauses?\b|\bsilen(ce|ces|t)\b|\bdead air\b/i],
  ['fillers', /\bfillers?\b|\b(ums?|umm+|uhs?|uhh+|ahs?|ahh+|erm+|hmm+)\b|\byou know\b/i],
  ['retakes', /\bre-?takes?\b|\brepeated takes?\b|\bfalse starts?\b|\bsaid (it|that)? ?again\b|\b(bad|first|botched|failed) (take|attempt)\b|\bflubs?\b|\bbotch(ed)?\b|\bfumbl(e|ed)\b|\bstumbl(e|ed)\b/i],
  ['music', /\bmusic\b|\bsoundtrack\b|\bbackground (track|audio)\b|\bduck\b/i],
  ['beats', /\bbeat\b|\bbeats\b|\brhythm\b/i],
  ['assemble', /\bteaser\b|\btrailer\b|\bhighlights?\b|\bfirst cut\b|\brough cut\b|\brecap\b|\bsizzle\b|\bsummary\b|\b\d+[- ]?(s|sec|secs|seconds?|min|minutes?)\b.*\b(from|of)\b/i],
  ['clips', /\b(trim|cut|split|shorten|move|reorder|swap|delete|remove|duplicate|speed|slow|faster|louder|quieter|volume|mute|first|last|before|after|play|plays)\b/i],
];

export interface Route {
  intents: Intent[];
  /** the request needs a plan: several jobs, or a judgment job like a teaser */
  needsPlan: boolean;
  /** how the intents were found */
  via: 'rules' | 'model' | 'question';
}

/** Intents that change the timeline in their own way; two or more of them make a compound job. */
const JOBS: Intent[] = ['export', 'canvas', 'captions', 'title', 'pauses', 'fillers', 'retakes', 'music', 'beats', 'assemble'];

/** Words that make a new cut out of footage on their own; a bare "N seconds from/of" does not. */
const STRONG_ASSEMBLE = /\bteaser\b|\btrailer\b|\bhighlights?\b|\bfirst cut\b|\brough cut\b|\brecap\b|\bsizzle\b|\bsummary\b/i;
/** Words for taking a part off or out of what is already on the timeline. */
const CUTTING = /\b(trim|shorten|lose|chop|cut|drop|remove|delete|take (it |that |this )?(off|out)|get rid)\b/i;

export function routeByRules(message: string): Intent[] {
  let found = RULES.filter(([, re]) => re.test(message)).map(([i]) => i);
  // "cut the first 2 seconds of it" names a duration but is an edit of a clip, not a new cut from footage
  if (found.includes('assemble') && !STRONG_ASSEMBLE.test(message) && CUTTING.test(message)) {
    found = found.filter((i) => i !== 'assemble');
    if (!found.includes('clips')) found.push('clips');
  }
  // "captions bigger": the style words belong to captions, not to a clip edit
  const hasSpecific = found.some((i) => i !== 'clips');
  return hasSpecific ? found.filter((i) => i !== 'clips' || /\b(trim|shorten|split|move|reorder|swap|delete|duplicate|slow|slower|speed|faster|mute)\b/i.test(message)) : found;
}

/** Words that alone do not make a clip edit ("first", "after" appear in all kinds of requests): such a match is not trusted. */
const WEAK = /^(first|last|before|after|play|plays)$/i;

export function isConfident(message: string, intents: Intent[]): boolean {
  if (intents.length === 0) return false;
  if (intents.length === 1 && intents[0] === 'clips') {
    const verbs = message.match(/\b(trim|cut|split|shorten|move|reorder|swap|delete|remove|duplicate|speed|slow|faster|louder|quieter|volume|mute|first|last|before|after|plays?)\b/gi) ?? [];
    return verbs.some((v) => !WEAK.test(v));
  }
  return true;
}

export function needsPlan(intents: Intent[]): boolean {
  if (intents.includes('assemble')) return true;
  // pauses + fillers + retakes is one "clean up the speech" job; count it once
  const jobs = new Set(intents.filter((i) => JOBS.includes(i)).map((i) => (i === 'fillers' || i === 'retakes' ? 'pauses' : i)));
  return jobs.size >= 2;
}

export const isQuestion = (message: string): boolean => /\?\s*$/.test(message.trim()) && /^(what|how|why|when|where|which|who|can you|could you explain|is |are |does |do you|tell me)/i.test(message.trim());

/** Follow-ups like "make them bigger" point at what the last turns talked about. */
const CAPTION_STYLE = /\b(bigger|larger|smaller|font|size|colou?r|higher|lower|top|bottom|bolder)\b/i;

export function routeNoModel(message: string, recentText = ''): Route | null {
  const intents = routeByRules(message);
  if (intents.length === 0 && CAPTION_STYLE.test(message) && /captions?/i.test(recentText)) intents.push('captions');
  if (isConfident(message, intents)) return { intents, needsPlan: needsPlan(intents), via: 'rules' };
  if (isQuestion(message)) return { intents: ['question'], needsPlan: false, via: 'question' };
  return null;
}

/** What the model must understand to pick intents when no rule fires. */
export const ROUTER_PROMPT = `Classify the video-editing request into the intents it needs (up to three). Intents:
- pauses: cut silences, dead air, awkward gaps where nobody is speaking
- fillers: cut filler words (um, uh, you know)
- retakes: drop a flubbed or repeated take
- captions: add or restyle subtitles
- title: add a title or text card
- music: add background music or set how loud it is under speech
- beats: cut the video to the beat of music
- canvas: change the shape (vertical, square, 16:9) or frame rate
- export: render or save the finished video
- clips: any change to clips already on the timeline: trim or shorten a clip, cut the start or end off, cut a stretch out, split, delete, move, reorder, speed up or slow down, change the volume
- assemble: ONLY building a new cut by choosing parts of the footage by what they are about (teaser, highlights, summary, "pull N seconds about X"); never for trimming or cutting a part off
- undo / redo: take back or restore the last change
- question: the user only asks something and wants no edit`;

const TOOLSETS: Record<Intent, string[]> = {
  undo: ['undo', 'redo'],
  redo: ['redo', 'undo'],
  export: ['exportVideo'],
  canvas: ['setCanvas'],
  captions: ['addCaptions', 'styleCaptions'],
  title: ['addTitle'],
  pauses: ['removePauses'],
  fillers: ['removeFillers'],
  retakes: ['removeRetakes'],
  music: ['addMusic'],
  beats: ['cutToBeat', 'addMusic'],
  clips: ['trimClip', 'splitClip', 'deleteClips', 'moveClip', 'setClipProps', 'removeSection', 'addMedia'],
  assemble: ['readTranscript', 'searchTranscript', 'assembleSequence'],
  question: [],
};

/** 3-8 facade tools for the intents (never the whole catalog). */
export function toolsetFor(intents: Intent[]): string[] {
  const names: string[] = [];
  for (const i of intents) for (const t of TOOLSETS[i]) if (!names.includes(t)) names.push(t);
  return names.slice(0, 8);
}

const ACTIONS = /\b(split|delete|remove|trim|move|cut|add|duplicate|mute|speed|slow|swap|reorder|put|shorten|extend)\b/gi;

/** The request names more than one action ("trim it, then slow it down"). */
export function looksCompound(message: string): boolean {
  const verbs = new Set((message.match(ACTIONS) ?? []).map((v) => v.toLowerCase()));
  return verbs.size >= 2 && /\b(and|then|also|after that)\b|[,;]/i.test(message);
}
