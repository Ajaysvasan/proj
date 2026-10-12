from fastapi import APIRouter, FastAPI

from .services.user_service import UserService

api = FastAPI()
router = APIRouter(prefix="/items")


@router.get("/{item_id}")
def read_item(item_id: int):
    service = UserService()
    return service.get_user(item_id)


api.include_router(router, prefix="/v1")
