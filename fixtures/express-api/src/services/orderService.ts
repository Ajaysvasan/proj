import { OrderRepository } from '../repositories/orderRepository';

export interface Order {
  id: string;
  total: number;
}

export class OrderService {
  constructor(private readonly repo: OrderRepository = new OrderRepository()) {}

  list(): Order[] {
    return this.repo.all();
  }

  find(id: string): Order {
    const order = this.repo.byId(id);
    if (!order) {
      throw new Error(`order ${id} not found`);
    }
    return order;
  }
}
