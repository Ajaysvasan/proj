from .models import Order


def order_detail(request, order_id):
    return Order.objects.get(pk=order_id)
