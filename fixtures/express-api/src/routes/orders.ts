import { Router, Request, Response } from 'express';
import { OrderService } from '../services/orderService';

const router = Router();
const orders = new OrderService();

router.get('/', (req: Request, res: Response) => {
  res.json(orders.list());
});

router.get('/:orderId', (req: Request, res: Response) => {
  res.json(orders.find(req.params.orderId));
});

export default router;
