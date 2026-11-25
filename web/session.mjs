// SPDX-License-Identifier: GPL-2.0-or-later
// A session script (tools/oracle-wine/README.md: the JSON a person writes) as the text the runtime's session
// player reads from SW_SESSION_STEPS (rt/windows-x86/input.c): steps separated by ';'.
export function sessionSteps(session) {
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
