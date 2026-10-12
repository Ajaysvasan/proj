from django.conf.urls import url
from django.urls import path

from . import views

urlpatterns = [
    url(r"^products/(?P<pk>\d+)/$", views.ProductView.as_view(), name="product"),
    path("cart/", views.cart),
]
