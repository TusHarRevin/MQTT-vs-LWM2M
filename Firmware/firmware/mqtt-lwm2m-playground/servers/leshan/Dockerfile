# Eclipse Leshan demo server (LwM2M server with web UI and REST API).
FROM eclipse-temurin:21-jre
ARG LESHAN_VERSION=2.0.0-M18
ADD https://repo1.maven.org/maven2/org/eclipse/leshan/leshan-demo-server/${LESHAN_VERSION}/leshan-demo-server-${LESHAN_VERSION}-jar-with-dependencies.jar /opt/leshan/leshan-demo-server.jar
EXPOSE 8080 5683/udp 5684/udp
ENTRYPOINT ["java", "-Xmx256m", "-jar", "/opt/leshan/leshan-demo-server.jar"]
