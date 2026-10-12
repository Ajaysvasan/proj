from api.pets import list_pets


def list_pets_handler(event, context):
    return {"statusCode": 200, "body": list_pets()}


def create_pet_handler(event, context):
    return {"statusCode": 201}
