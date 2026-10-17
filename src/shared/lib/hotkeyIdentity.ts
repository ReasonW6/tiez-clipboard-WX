const aliases: Record<string, string> = {
  CONTROL: "CTRL", MENU: "ALT", OPTION: "ALT",
  WIN: "META", SUPER: "META", COMMAND: "META", CMD: "META",
  RETURN: "ENTER", SPACEBAR: "SPACE", MBUTTON: "MOUSEMIDDLE",
  ARROWLEFT: "LEFT", ARROWRIGHT: "RIGHT", ARROWUP: "UP", ARROWDOWN: "DOWN"
};

export const hotkeyIdentity = (hotkey: string) => hotkey.split("+")
  .map(part => part.trim().toUpperCase()).filter(Boolean)
  .map(part => aliases[part] || part).sort().join("+");
