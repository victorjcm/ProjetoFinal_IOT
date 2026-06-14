import asyncio
import sqlite3
from datetime import datetime
import aiocoap.resource as resource
import aiocoap
import paho.mqtt.client as mqtt

# ==========================================
# 1. CREDENCIAIS E CONFIGURAÇÕES DE REDE
# ==========================================
IO_USERNAME = "Victorjcm"
IO_KEY      = "xxx"

# Endereços de Feeds
FEED_LUMINOSIDADE   = f"{IO_USERNAME}/feeds/luminosidade"
FEED_LIMITE_LUZ     = f"{IO_USERNAME}/feeds/limite-luz"
FEED_MODO_OPERACAO  = f"{IO_USERNAME}/feeds/modo-operacao"
FEED_STATUS_CORTINA = f"{IO_USERNAME}/feeds/status-cortina"
FEED_BOTAO_VIRTUAL  = f"{IO_USERNAME}/feeds/botao-virtual"

# --->IP DA CORTINA<---
IP_ESP_CORTINA = "192.168.0.251" 

# Variáveis globais
loop = None
protocolo_coap = None    # canal de comunoicação
limite_luz = 1000        
modo_atual = "MANUAL"    
cortina_aberta = True    

# ==========================================
# 2. CLIENTE CoAP (Reutilizando o canal)
# ==========================================
async def enviar_comando_coap(comando):
    """Envia ordens usando o contexto global sem criar sockets novos"""
    try:
        request = aiocoap.Message(code=aiocoap.PUT, payload=comando.encode('utf-8'))
        request.set_request_uri(f'coap://{IP_ESP_CORTINA}/cortina')
        
        # Usa o protocolo_coap global estabelecido
        await protocolo_coap.request(request).response
        print(f"[CoAP -> Cortina] Comando '{comando}' enviado com sucesso!")
    except Exception as e:
        print(f"[CoAP -> Cortina] Falha ao contatar o dispositivo: {e}")

# ==========================================
# 3. ESCUTANDO A NUVEM (MQTT)
# ==========================================
def on_connect(client, userdata, flags, rc):
    print("\n[MQTT] Conectado ao Adafruit IO com sucesso!")
    client.subscribe(FEED_BOTAO_VIRTUAL)
    client.subscribe(FEED_MODO_OPERACAO)
    client.subscribe(FEED_LIMITE_LUZ)

def on_message(client, userdata, msg):
    global limite_luz, modo_atual, cortina_aberta
    
    topic = msg.topic
    payload = msg.payload.decode('utf-8')
    print(f"[NUVEM -> Gateway] Atualização: {topic.split('/')[-1]} = {payload}")

    if topic == FEED_BOTAO_VIRTUAL:
        if modo_atual == "AUTOMATICO":
            modo_atual = "MANUAL"
            client.publish(FEED_MODO_OPERACAO, "MANUAL")
            asyncio.run_coroutine_threadsafe(enviar_comando_coap("AUTO_OFF"), loop)
            
        cortina_aberta = not cortina_aberta
        client.publish(FEED_STATUS_CORTINA, "1" if cortina_aberta else "0")
        asyncio.run_coroutine_threadsafe(enviar_comando_coap("INVERTER"), loop)
        
    elif topic == FEED_MODO_OPERACAO:
        modo_atual = payload
        cmd_coap = "AUTO_ON" if payload == "AUTOMATICO" else "AUTO_OFF"
        asyncio.run_coroutine_threadsafe(enviar_comando_coap(cmd_coap), loop)

    elif topic == FEED_LIMITE_LUZ:
        limite_luz = int(payload)
        print(f"[Ajuste] Novo limite de luminosidade definido para: {limite_luz}")

client = mqtt.Client()
client.username_pw_set(IO_USERNAME, IO_KEY)
client.on_connect = on_connect
client.on_message = on_message
client.connect("io.adafruit.com", 1883, 60)
client.loop_start()

# ==========================================
# 4. BANCO DE DADOS LOCAL
# ==========================================
conn = sqlite3.connect('banco_automacao.db')
cursor = conn.cursor()
cursor.execute('''
    CREATE TABLE IF NOT EXISTS leituras_luz (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        data_hora TEXT,
        valor INTEGER
    )
''')
conn.commit()

# ==========================================
# 5. SERVIDOR CoAP
# ==========================================
class SensorResource(resource.Resource):
    async def render_post(self, request):
        global cortina_aberta
        
        payload = request.payload.decode('utf-8')
        valor_luz = int(payload)
        print(f"\n[Sensor -> Gateway] Leitura LDR recebida: {valor_luz}")

        agora = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        cursor.execute("INSERT INTO leituras_luz (data_hora, valor) VALUES (?, ?)", (agora, valor_luz))
        conn.commit()

        client.publish(FEED_LUMINOSIDADE, valor_luz)
        
        if modo_atual == "AUTOMATICO":
            if valor_luz > limite_luz and cortina_aberta:
                print("[Automação] Luz alta! Fechando cortina.")
                cortina_aberta = False
                client.publish(FEED_STATUS_CORTINA, "0")
                asyncio.create_task(enviar_comando_coap("INVERTER"))
                
            elif valor_luz <= limite_luz and not cortina_aberta:
                print("[Automação] Ambiente escureceu! Abrindo cortina.")
                cortina_aberta = True
                client.publish(FEED_STATUS_CORTINA, "1")
                asyncio.create_task(enviar_comando_coap("INVERTER"))

        return aiocoap.Message(code=aiocoap.CHANGED, payload=b"OK")

# ==========================================
# 6. EXECUÇÃO PRINCIPAL
# ==========================================
async def main():
    global loop, protocolo_coap
    loop = asyncio.get_running_loop()

    root = resource.Site()
    root.add_resource(['sensor_luz'], SensorResource())

    # Bind em 0.0.0.0 aceita pacotes de QUALQUER IP do seu computador
    protocolo_coap = await aiocoap.Context.create_server_context(root, bind=('0.0.0.0', 5683))
    
    print("==================================================")
    print(" HUB CENTRAL ATIVO! Monitorando rede local e nuvem")
    print("==================================================")
    
    await loop.create_future()

if __name__ == "__main__":
    import logging
    logging.getLogger("coap-server").setLevel(logging.ERROR)
    asyncio.run(main())