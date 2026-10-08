// Generates the eval media in ./out and the ground-truth ./manifest.json.
//   node apps/desktop/evals/fixtures/generate.mjs [--force]
// Speech comes from Windows' built-in System.Speech TTS, video/music from ffmpeg lavfi
// sources, so nothing is downloaded. Every spoken segment is synthesized separately and laid
// out with explicit silences, so pause / filler / retake / topic times are exact, not guessed.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'out');
const TTS = join(OUT, 'tts');
const BIN = join(HERE, '..', '..', 'bin', 'win32-x64');
const FFMPEG = join(BIN, 'ffmpeg.exe');
const FFPROBE = join(BIN, 'ffprobe.exe');
const FONT = 'C\\:/Windows/Fonts/arial.ttf';
const VOICE = 'Microsoft David Desktop';
const SAMPLE_RATE = 44100;
/** natural silence between two consecutive sentences / words of the same speaker */
const GAP_SEC = 0.18;
const FORCE = process.argv.includes('--force');

mkdirSync(TTS, { recursive: true });

function run(cmd, args, label) {
  const res = spawnSync(cmd, args, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, windowsHide: true });
  if (res.status !== 0) throw new Error(`${label ?? cmd} failed (${res.status}): ${(res.stderr || res.stdout || '').slice(-1500)}`);
  return res.stdout;
}
const ffmpeg = (args, label) => run(FFMPEG, ['-hide_banner', '-loglevel', 'error', '-y', ...args], label ?? 'ffmpeg');
const probeDuration = (file) => Number(run(FFPROBE, ['-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', file]).trim());
const round = (n) => Math.round(n * 1000) / 1000;

// ---------------- scripts ----------------
// an item is a sentence ({text}), a filler word ({filler}) or an explicit pause ({pause} seconds)

const interview = [
  { text: 'Hi everyone, thanks for having me on the show today.' },
  { pause: 1.6 },
  { text: 'I have been building video tools for about ten years now.' },
  { text: 'We started out making simple desktop software' },
  { filler: 'um' },
  { text: 'and over time it grew into something much bigger.' },
  { pause: 2.2 },
  { text: 'The biggest lesson I learned is that speed matters more than polish.' },
  { filler: 'uh' },
  { text: 'People want to try an idea right away, not wait for a render bar.' },
  { pause: 1.4 },
  { text: 'So the reason we started this company was, well', retake: 'abandoned' },
  { pause: 1.1 },
  { text: 'So the reason we started this company was to make editing feel effortless.', retake: 'clean' },
  { text: 'Every feature we ship has to pass one test.' },
  { filler: 'you know' },
  { text: 'Can a beginner finish a video in ten minutes?' },
  { pause: 2.0 },
  { text: 'If the answer is no, we go back and simplify it.' },
  { text: 'That is the whole philosophy, and it has not changed.' },
  { filler: 'um' },
  { text: 'Looking ahead, we are working on better tools for audio cleanup.' },
  { text: 'We also want to make collaboration feel as simple as sharing a link.' },
  { pause: 1.3 },
  { text: 'Thanks again for listening, and see you next time.' },
];

const TOPICS = [
  {
    id: 'pricing',
    title: 'Pricing',
    color: '0x1d3557',
    sentences: [
      'Let us start with pricing, because it is the first question everyone asks.',
      'The starter plan costs nine dollars a month and includes the full editor.',
      'The team plan is nineteen dollars per seat and adds shared libraries.',
      'We do not charge extra for exports, so you can render as many videos as you like.',
      'Annual billing saves you twenty percent compared to paying monthly.',
      'Students and teachers get half off the starter plan with a school email.',
      'If you outgrow a plan, you can upgrade at any time and only pay the difference.',
      'There are no hidden fees, no watermarks, and no surprise price increases.',
    ],
  },
  {
    id: 'customer',
    title: 'Customer story',
    color: '0x6a994e',
    sentences: [
      'Now I want to tell you about Maria, who runs a small bakery in Lisbon.',
      'Maria had never edited a video before she found our app last spring.',
      'She filmed her sourdough process on her phone and dropped the clips into the timeline.',
      'In one afternoon she cut a two minute story about her family recipe.',
      'Her video was shared by thousands of food lovers and her bakery sold out in three days.',
      'Maria told us she now posts a new video every single week.',
      'She says the captions feature helped her reach customers who watch with the sound off.',
      'Today her bakery has doubled in size and she hired two apprentices.',
    ],
  },
  {
    id: 'demo',
    title: 'Product demo',
    color: '0xbc4749',
    sentences: [
      'Next, let me walk you through the product itself.',
      'You begin by importing your footage, and the app analyzes every clip automatically.',
      'The timeline shows your video tracks on top and your audio tracks underneath.',
      'To trim a clip, you simply drag its edge to the moment you want.',
      'You can split a clip at the playhead and delete the parts you do not need.',
      'The inspector lets you adjust scale, position, opacity, and volume.',
      'When you are happy with the edit, the export button renders the final movie.',
      'The whole workflow runs locally on your computer, so your footage never leaves your machine.',
    ],
  },
  {
    id: 'hiring',
    title: 'Hiring',
    color: '0xf4a261',
    sentences: [
      'Finally, a quick word about hiring, because we are growing fast.',
      'We are looking for two senior engineers who love graphics programming.',
      'We also need a product designer to shape the look of the editor.',
      'Our team works remotely across five time zones and meets once a quarter.',
      'Every employee receives equity, a learning budget, and flexible working hours.',
      'If you are interested, send us a short note and a link to something you built.',
      'We read every application personally and reply within a week.',
      'Thank you so much for your time, and have a wonderful rest of your day.',
    ],
  },
];
const TOPIC_GAP_SEC = 1.0;

const talk = TOPICS.flatMap((t, ti) => [
  ...(ti > 0 ? [{ pause: TOPIC_GAP_SEC, boundary: true }] : []),
  ...t.sentences.map((text) => ({ text, topic: t.id })),
]);

// ---------------- speech synthesis ----------------

/** spoken string -> wav path (a repeated "um" is only synthesized once) */
function wavTable(items) {
  const unique = new Map();
  for (const it of items) {
    const text = it.text ?? it.filler;
    if (text && !unique.has(text)) unique.set(text, join(TTS, `s${unique.size}-${Buffer.from(text).toString('hex').slice(0, 12)}.wav`));
  }
  return unique;
}

/** how a filler is actually voiced (plain "um" is read as a letter pair by some voices) */
/** TTS pads every utterance with silence; strip both ends so the laid-out pauses are the only silence */
const TRIM = 'silenceremove=start_periods=1:start_threshold=-45dB,areverse,silenceremove=start_periods=1:start_threshold=-45dB,areverse';
const SPOKEN = { um: 'Umm.', uh: 'Uh.' };

function runTts(table) {
  const jobs = [];
  for (const [text, file] of table) {
    if (FORCE || !existsSync(file)) jobs.push({ text: SPOKEN[text] ?? text, file: `${file}.raw.wav` });
  }
  if (jobs.length === 0) return;
  const jobsPath = join(TTS, 'jobs.json');
  writeFileSync(jobsPath, JSON.stringify(jobs));
  const ps = join(TTS, 'tts.ps1');
  writeFileSync(
    ps,
    [
      'Add-Type -AssemblyName System.Speech',
      '$s = New-Object System.Speech.Synthesis.SpeechSynthesizer',
      `$s.SelectVoice('${VOICE}')`,
      '$s.Rate = 0',
      `$jobs = Get-Content -Raw -Encoding UTF8 '${jobsPath}' | ConvertFrom-Json`,
      'foreach ($j in $jobs) { $s.SetOutputToWaveFile($j.file); $s.Speak($j.text); $s.SetOutputToNull() }',
    ].join('\r\n'),
  );
  run('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ps], 'tts');
  for (const j of jobs) {
    const final = j.file.replace(/\.raw\.wav$/, '');
    ffmpeg(['-i', j.file, '-af', TRIM, '-ar', String(SAMPLE_RATE), '-ac', '1', '-c:a', 'pcm_s16le', final], 'wav convert');
    rmSync(j.file);
  }
}

function silenceFile(sec) {
  const file = join(TTS, `silence-${Math.round(sec * 1000)}.wav`);
  if (FORCE || !existsSync(file)) ffmpeg(['-f', 'lavfi', '-i', `anullsrc=r=${SAMPLE_RATE}:cl=mono`, '-t', String(sec), '-c:a', 'pcm_s16le', file], 'silence');
  return file;
}

/** Lay items out on a timeline: returns the wav parts to concatenate and the placed segments with exact times. */
function layout(items, wavs) {
  const parts = [];
  const segments = [];
  let t = 0;
  const push = (file) => {
    parts.push(file);
    t += probeDuration(file);
  };
  let prevSpoke = false;
  for (const it of items) {
    if (it.pause !== undefined) {
      // an explicit pause replaces the natural gap
      const start = t;
      push(silenceFile(it.pause));
      segments.push({ kind: 'pause', startSec: round(start), endSec: round(t), ...(it.boundary ? { boundary: true } : {}) });
      prevSpoke = false;
      continue;
    }
    if (prevSpoke) push(silenceFile(GAP_SEC));
    const text = it.text ?? it.filler;
    const start = t;
    push(wavs.get(text));
    segments.push({
      kind: it.filler ? 'filler' : 'speech',
      text,
      startSec: round(start),
      endSec: round(t),
      ...(it.retake ? { retake: it.retake } : {}),
      ...(it.topic ? { topic: it.topic } : {}),
    });
    prevSpoke = true;
  }
  return { parts, segments, durationSec: t };
}

function concatFiles(parts, outFile, extra) {
  const list = `${outFile}.txt`;
  writeFileSync(list, parts.map((p) => `file '${p.replace(/\\/g, '/')}'`).join('\n'));
  ffmpeg(['-f', 'concat', '-safe', '0', '-i', list, ...extra, outFile], 'concat');
}

// ---------------- video ----------------

const label = (text, size = 64, y = '(h-text_h)/2') =>
  `drawtext=fontfile='${FONT}':text='${text}':fontsize=${size}:fontcolor=white:borderw=3:bordercolor=black:x=(w-text_w)/2:y=${y}`;

// ---------------- build ----------------

const manifest = { generatedAt: new Date().toISOString(), voice: VOICE, fixtures: {} };

const table = wavTable([...interview, ...talk]);
runTts(table);

// interview.mp4
{
  const { parts, segments, durationSec } = layout(interview, table);
  const wav = join(OUT, 'interview.wav');
  concatFiles(parts, wav, ['-c:a', 'pcm_s16le']);
  ffmpeg(
    [
      '-f', 'lavfi', '-i', `testsrc2=s=1280x720:r=30:d=${round(durationSec + 1)}`,
      '-i', wav,
      '-vf', `${label('INTERVIEW', 72, '40')},format=yuv420p`,
      '-t', String(round(durationSec)),
      '-c:v', 'libopenh264', '-b:v', '2M', '-c:a', 'aac', '-b:a', '128k',
      join(OUT, 'interview.mp4'),
    ],
    'interview mux',
  );
  const pauses = segments.filter((s) => s.kind === 'pause').map((s) => ({ startSec: s.startSec, endSec: s.endSec, durSec: round(s.endSec - s.startSec) }));
  const abandoned = segments.find((s) => s.retake === 'abandoned');
  const clean = segments.find((s) => s.retake === 'clean');
  manifest.fixtures['interview.mp4'] = {
    durationSec: round(probeDuration(join(OUT, 'interview.mp4'))),
    kind: 'speech',
    segments,
    pauses,
    totalPauseSec: round(pauses.reduce((a, p) => a + p.durSec, 0)),
    fillers: segments.filter((s) => s.kind === 'filler').map((s) => ({ word: s.text, startSec: s.startSec, endSec: s.endSec })),
    retake: {
      abandoned: { text: abandoned.text, startSec: abandoned.startSec, endSec: abandoned.endSec },
      clean: { text: clean.text, startSec: clean.startSec, endSec: clean.endSec },
    },
  };
}

// talk.mp4: one coloured, labelled block of video per topic (so scene detection sees the topics too)
{
  const { parts, segments, durationSec } = layout(talk, table);
  const wav = join(OUT, 'talk.wav');
  concatFiles(parts, wav, ['-c:a', 'pcm_s16le']);
  const topics = TOPICS.map((t) => {
    const spoken = segments.filter((s) => s.topic === t.id);
    return { id: t.id, title: t.title, startSec: spoken[0].startSec, endSec: spoken[spoken.length - 1].endSec };
  });
  // video block boundaries sit in the middle of each topic gap
  const bounds = [0, ...topics.slice(1).map((t, i) => round((topics[i].endSec + t.startSec) / 2)), round(durationSec)];
  const blocks = TOPICS.map((t, i) => {
    const file = join(TTS, `talk-block-${t.id}.mp4`);
    ffmpeg(
      [
        '-f', 'lavfi', '-i', `color=c=${t.color}:s=1280x720:r=30:d=${round(bounds[i + 1] - bounds[i])}`,
        '-vf', `${label(t.title.toUpperCase(), 80)},format=yuv420p`,
        '-c:v', 'libopenh264', '-b:v', '2M', file,
      ],
      'talk block',
    );
    return file;
  });
  const list = join(TTS, 'talk-blocks.txt');
  writeFileSync(list, blocks.map((b) => `file '${b.replace(/\\/g, '/')}'`).join('\n'));
  ffmpeg(
    ['-f', 'concat', '-safe', '0', '-i', list, '-i', wav, '-c:v', 'copy', '-c:a', 'aac', '-b:a', '128k', '-t', String(round(durationSec)), join(OUT, 'talk.mp4')],
    'talk mux',
  );
  manifest.fixtures['talk.mp4'] = { durationSec: round(probeDuration(join(OUT, 'talk.mp4'))), kind: 'speech', segments, topics };
}

// b-roll: three visually distinct, silent clips
const BROLL = [
  { name: 'broll-a.mp4', sec: 5, color: '0xd62828', text: 'BROLL A', motion: 'x=mod(t*160\\,w):y=100' },
  { name: 'broll-b.mp4', sec: 6, color: '0x2a9d8f', text: 'BROLL B', motion: 'x=100:y=mod(t*90\\,h)' },
  { name: 'broll-c.mp4', sec: 7, color: '0x3a0ca3', text: 'BROLL C', motion: 'x=mod(t*60\\,w):y=mod(t*60\\,h)' },
];
for (const b of BROLL) {
  ffmpeg(
    [
      '-f', 'lavfi', '-i', `color=c=${b.color}:s=1280x720:r=30:d=${b.sec}`,
      '-vf', `${label(b.text, 96)},drawbox=${b.motion}:w=120:h=120:color=white@0.9:t=fill,format=yuv420p`,
      '-an', '-c:v', 'libopenh264', '-b:v', '2M', join(OUT, b.name),
    ],
    b.name,
  );
  manifest.fixtures[b.name] = { durationSec: round(probeDuration(join(OUT, b.name))), kind: 'broll', label: b.text, hasAudio: false };
}

// music.mp3: 120 BPM click + kick, every 4th beat accented, first beat at t=0
{
  const bpm = 120;
  const period = 60 / bpm;
  const expr = `(0.5*sin(2*PI*1500*t)*exp(-90*mod(t,${period}))+0.8*sin(2*PI*60*t)*exp(-14*mod(t,${period})))*(1+0.5*lt(mod(t,${period * 4}),${period}))*0.6`;
  ffmpeg(['-f', 'lavfi', '-i', `aevalsrc='${expr}':s=${SAMPLE_RATE}:d=60`, '-c:a', 'libmp3lame', '-b:a', '128k', join(OUT, 'music.mp3')], 'music');
  manifest.fixtures['music.mp3'] = {
    durationSec: round(probeDuration(join(OUT, 'music.mp3'))),
    kind: 'music',
    bpm,
    beatPeriodSec: period,
    firstBeatSec: 0,
    barEveryBeats: 4,
  };
}

writeFileSync(join(HERE, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(
  'fixtures ready:',
  Object.entries(manifest.fixtures)
    .map(([k, v]) => `${k} ${v.durationSec}s`)
    .join(', '),
);
