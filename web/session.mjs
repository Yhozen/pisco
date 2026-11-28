// SPDX-License-Identifier: GPL-2.0-or-later
// A session script (tools/oracle-wine/README.md: the JSON a person writes) as the text the runtime's session
// player reads from SW_SESSION_STEPS (rt/input.c): steps separated by ';'.
// Schema 1 (docs/spec/sessions.md): its keys, and each step's (its action, and what may go with it)
export const SESSION_KEYS = ['schema', 'app', 'about', 'steps', 'random'];
export const STEPS = { wait: [], focus: [], move: ['ms'], click: ['ms'], button: ['down'], key: ['ms', 'down'], shot: [] };
// A session as schema 1 says, or an error that says what is wrong with it
export function checkSession(session) {
  if (session?.schema !== 1) throw new Error(`session: schema ${JSON.stringify(session?.schema)}: this host reads sessions of schema 1 (docs/spec/sessions.md)`);
  const bad = Object.keys(session).find((k) => !SESSION_KEYS.includes(k));
  if (bad) throw new Error(`session: ${bad} is not a key of a session (${SESSION_KEYS.join(', ')})`);
  if (typeof session.app !== 'string') throw new Error('session: no app (the name of the app it is for)');
  if (!Array.isArray(session.steps)) throw new Error('session: no steps');
  session.steps.forEach((s, i) => {
    const acts = Object.keys(s).filter((k) => k in STEPS);
    if (acts.length !== 1) throw new Error(`session: step ${i + 1} ${JSON.stringify(s)}: one of ${Object.keys(STEPS).join(', ')}`);
    const extra = Object.keys(s).find((k) => k !== acts[0] && !STEPS[acts[0]].includes(k));
    if (extra) throw new Error(`session: step ${i + 1} ${JSON.stringify(s)}: ${extra} does not go with ${acts[0]}`);
  });
  return session;
}
export function sessionSteps(session) {
  checkSession(session);
  const one = (s) => {
    if ('wait' in s) return `wait ${s.wait | 0}`;
    if ('focus' in s) return 'focus';
    if ('move' in s) return `move ${s.move[0] | 0} ${s.move[1] | 0} ${s.ms | 0}`;
    if ('click' in s) return `click ${s.click} ${s.ms | 0}`;
    if ('button' in s) return `button ${s.button} ${s.down ? 1 : 0}`;
    if ('key' in s) return 'down' in s ? `${s.down ? 'keydown' : 'keyup'} ${s.key}` : `key ${s.key} ${s.ms | 0}`;
    if ('shot' in s) return `shot ${s.shot}`;
    throw new Error(`session: unknown step ${JSON.stringify(s)}`);
  };
  const steps = session.steps.map(one);
  const bad = steps.find((t) => /[;\n]/.test(t));
  if (bad) throw new Error(`session: a step cannot contain ';' (${bad})`);
  return steps.join(';');
}
// What else a session sets for the runtime: `"random": ["aee69d51", ...]`, the values the app's random source
// (rand_s) answers first (SW_RANDOM): the ones a Wine run of the app got, so that its random world is that run's
export function sessionEnv(session) {
  checkSession(session);
  const env = {};
  if (session.random) env.SW_RANDOM = session.random.map((v) => String(v)).join(',');
  return env;
}
