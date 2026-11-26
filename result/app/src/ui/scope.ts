// A screen's lifetime. main.ts show() builds every tab inside a fresh scope and disposes the previous one when the
// tab is replaced (the panel runs for days: listeners and timers of replaced tabs must not pile up). Code that runs
// while a screen is being built registers its unsubscribers with onDispose(); code that runs later captures the
// scope with currentScope() first. Screens built outside a scope (tests calling a screen directly) get no disposal.

export interface Scope {
  /** run `off` when the scope is disposed (at once when it already is) */
  add(off: () => void): void;
  dispose(): void;
  readonly disposed: boolean;
}

export function createScope(): Scope {
  let offs: (() => void)[] = [];
  let disposed = false;
  return {
    add(off) {
      if (disposed) off();
      else offs.push(off);
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      const run = offs;
      offs = [];
      for (const off of run) {
        try {
          off();
        } catch (e) {
          console.error(e);
        }
      }
    },
    get disposed() {
      return disposed;
    },
  };
}

let ambient: Scope | null = null;

/** Build with `scope` as the current scope (restored afterwards, also when `build` throws). */
export function inScope<T>(scope: Scope, build: () => T): T {
  const prev = ambient;
  ambient = scope;
  try {
    return build();
  } finally {
    ambient = prev;
  }
}

/** The scope of the screen being built (null outside a build or for a screen built without one). */
export function currentScope(): Scope | null {
  return ambient;
}

/** Register `off` with the screen being built; false when there is none. */
export function onDispose(off: () => void): boolean {
  if (!ambient) return false;
  ambient.add(off);
  return true;
}

/**
 * Without a scope: true once `root()` was in the document (at a check, or just after the build) and has left it -
 * the metronome tab's rule for a screen built directly. Callers drop their listener at the first event after that.
 */
export function leftDocument(root: () => Element | null): () => boolean {
  let seen = false;
  const gone = () => {
    const el = root();
    if (el?.isConnected) seen = true;
    return seen && !el?.isConnected;
  };
  setTimeout(gone, 0);
  return gone;
}
