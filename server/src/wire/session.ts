let nextConnectionId = 1;
let nextServerRequestId = 1;

export class Session {
  readonly connectionId: number;

  constructor() {
    this.connectionId = nextConnectionId++;
  }

  allocRequestId(): number {
    return nextServerRequestId++;
  }
}
