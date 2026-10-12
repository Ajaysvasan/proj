import type { Order } from '../services/orderService';

export class OrderRepository {
  private orders: Order[] = [];

  all(): Order[] {
    return this.orders;
  }

  byId(id: string): Order | undefined {
    return this.orders.find((o) => o.id === id);
  }
}
