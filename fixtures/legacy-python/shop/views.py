from django.http import JsonResponse
from django.views import View


class ProductView(View):
    def get(self, request, pk):
        return JsonResponse({"pk": pk})

    def post(self, request, pk):
        return JsonResponse({"saved": pk})


def cart(request):
    return JsonResponse({"items": []})
