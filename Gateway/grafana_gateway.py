import asyncio
import sqlite3
import json
from datetime import datetime
import aiocoap.resource as resource
import aiocoap
from aiohttp import web

# ==========================================
# 1. CONFIGURAÇÕES (Verifique seu IP!)
# ==========================================
IP_ESP_CORTINA = "192.168.1.111" 

loop = None
protocolo_coap = None  
limite_luz = 1000  
modo_atual = "AUTOMATICO"
cortina_aberta = False  

conn = sqlite3.connect("banco_automacao.db")
cursor = conn.cursor()
cursor.execute("CREATE TABLE IF NOT EXISTS leituras_luz (id INTEGER PRIMARY KEY AUTOINCREMENT, data_hora TEXT, valor INTEGER)")
conn.commit()

async def enviar_comando_coap(comando):
    try:
        request = aiocoap.Message(code=aiocoap.PUT, payload=comando.encode("utf-8"))
        request.set_request_uri(f"coap://{IP_ESP_CORTINA}/cortina")
        response = await protocolo_coap.request(request).response
        return response.payload.decode("utf-8")
    except Exception as e:
        print(f"[CoAP] Erro: {e}")
        return None

# ==========================================
# 2. SERVIDOR WEB PARA O GRAFANA (Com CORS)
# ==========================================
def get_cors_headers():
    return {"Access-Control-Allow-Origin": "*"}

async def handle_options(request):
    return web.Response(headers=get_cors_headers())

async def handle_comando_web(request):
    global modo_atual, cortina_aberta, limite_luz
    acao = request.query.get('acao')
    valor = request.query.get('valor')
    
    print(f"\n[Grafana] Ação recebida: {acao} | Valor: {valor}")
    
    if acao == "TOGGLE_CORTINA":
        novo_estado = "FECHAR" if cortina_aberta else "ABRIR"
        if modo_atual == "AUTOMATICO":
            modo_atual = "MANUAL"
            await enviar_comando_coap("AUTO_OFF")
        status = await enviar_comando_coap(novo_estado)
        if status: cortina_aberta = True if status == "ABERTA" else False
        return web.Response(text="OK", headers=get_cors_headers())

    elif acao == "TOGGLE_MODO":
        if modo_atual == "AUTOMATICO":
            modo_atual = "MANUAL"
            await enviar_comando_coap("AUTO_OFF")
        else:
            modo_atual = "AUTOMATICO"
            await enviar_comando_coap("AUTO_ON")
        return web.Response(text="OK", headers=get_cors_headers())
        
    elif acao == "SET_LIMITE":
        if valor:
            limite_luz = int(valor)
        return web.Response(text="OK", headers=get_cors_headers())

    return web.Response(status=400, text="Erro", headers=get_cors_headers())

async def handle_estado_web(request):
    estado = {
        "modo": modo_atual,
        "cortina": "ABERTA" if cortina_aberta else "FECHADA",
        "limite_luz": limite_luz
    }
    return web.json_response(estado, headers=get_cors_headers())

app = web.Application()
app.router.add_options('/api/comando', handle_options)
app.router.add_options('/api/estado', handle_options)
app.router.add_get('/api/comando', handle_comando_web)
app.router.add_get('/api/estado', handle_estado_web)

# ==========================================
# 3. SERVIDORES CoAP (LDR e Botão)
# ==========================================
class SensorResource(resource.Resource):
    async def render_post(self, request):
        global cortina_aberta
        valor_luz = int(request.payload.decode("utf-8"))
        
        agora = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        cursor.execute("INSERT INTO leituras_luz (data_hora, valor) VALUES (?, ?)", (agora, valor_luz))
        conn.commit()

        if modo_atual == "AUTOMATICO":
            if valor_luz > limite_luz and not cortina_aberta:
                status = await enviar_comando_coap("ABRIR")
                if status == "ABERTA" or status == "": cortina_aberta = True
            elif valor_luz <= limite_luz and cortina_aberta:
                status = await enviar_comando_coap("FECHAR")
                if status == "FECHADA" or status == "": cortina_aberta = False

        return aiocoap.Message(code=aiocoap.CHANGED, payload=b"OK")

class BotaoFisicoResource(resource.Resource):
    async def render_post(self, request):
        global modo_atual, cortina_aberta
        estado_fisico = request.payload.decode("utf-8")
        if modo_atual == "AUTOMATICO": modo_atual = "MANUAL"
        cortina_aberta = True if estado_fisico == "ABERTA" else False
        return aiocoap.Message(code=aiocoap.CHANGED, payload=b"OK")

# ==========================================
# 4. START
# ==========================================
async def main():
    global protocolo_coap
    root = resource.Site()
    root.add_resource(["sensor_luz"], SensorResource())
    root.add_resource(["botao_fisico"], BotaoFisicoResource())
    protocolo_coap = await aiocoap.Context.create_server_context(root, bind=("0.0.0.0", 5683))

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, '0.0.0.0', 8080)
    await site.start()
    
    print("==================================================")
    print(" GATEWAY 100% LOCAL RODANDO (Pronto para o Grafana)")
    print("==================================================")
    await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())