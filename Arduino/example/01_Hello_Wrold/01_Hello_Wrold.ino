void setup() {
  Serial.begin(115200);
}

void loop() {
   Serial.println("Hello, World!"); // 发送字符串并换行
   delay(1000);

   if (Serial.available() > 0) {
    char incomingByte = Serial.read();
    Serial.print("Received: ");
    Serial.println(incomingByte); // 将接收到的数据回显
  }
}
