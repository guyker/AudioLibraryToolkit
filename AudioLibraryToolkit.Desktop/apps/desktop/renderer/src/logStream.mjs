const logLimit = 200_000;

export function subscribeToLogs(bridge, onChange, onError) {
  let active = true;
  let snapshot = null;
  const pending = [];

  function apply(update) {
    if (update.revision <= snapshot.revision) return;
    snapshot = {
      revision: update.revision,
      text: (update.reset ? update.text : snapshot.text + update.text).slice(-logLimit),
    };
  }

  const unsubscribe = bridge.onLogUpdate((update) => {
    if (!active) return;
    if (!snapshot) {
      pending.push(update);
      return;
    }
    apply(update);
    onChange(snapshot.text);
  });
  void bridge.getLogs().then((initial) => {
    if (!active) return;
    snapshot = initial;
    for (const update of pending) apply(update);
    pending.length = 0;
    onChange(snapshot.text);
  }).catch((error) => {
    if (active) {
      active = false;
      unsubscribe();
      onError(error);
    }
  });

  return () => {
    active = false;
    unsubscribe();
  };
}
