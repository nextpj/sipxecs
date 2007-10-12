//
// Copyright (C) 2007 Pingtel Corp., certain elements licensed under a Contributor Agreement.  
// Contributors retain copyright to elements licensed under a Contributor Agreement.
// Licensed to the User under the LGPL license.
// 
//
// $$
////////////////////////////////////////////////////////////////////////
//////


// SYSTEM INCLUDES
#include <stdio.h>

// APPLICATION INCLUDES
#include <net/SipMessage.h>
#include <net/SipClient.h>
#include <net/SipMessageEvent.h>
#include <net/SipUserAgentBase.h>

#include <os/OsDateTime.h>
#include <os/OsDatagramSocket.h>
#include <os/OsSysLog.h>
#include <os/OsEvent.h>

#include <utl/XmlContent.h>

#define SIP_DEFAULT_RTT 500

#define LOG_TIME

// EXTERNAL FUNCTIONS
// EXTERNAL VARIABLES
// CONSTANTS

// All requests must contain at least 72 characters:
/*
X Y SIP/2.0 \n\r
i: A\n\r
f: B\n\r
t: C\n\r
c: 1\n\r
v: SIP/2.0/UDP D\n\r
l: 0 \n\r
\n\r

*/
// However to be tolerant of malformed messages we allow smaller:
#define MINIMUM_SIP_MESSAGE_SIZE 30
#define MAX_UDP_PACKET_SIZE (1024 * 64)

// STATIC VARIABLE INITIALIZATIONS

/* //////////////////////////// PUBLIC //////////////////////////////////// */

/* ============================ CREATORS ================================== */

// Constructor
SipClient::SipClient(OsSocket* socket) :
   OsTask("SipClient-%d"),
   clientSocket(socket),
   mSocketType(socket ? socket->getIpProtocol() : OsSocket::UNKNOWN),
   sipUserAgent(NULL),
   mRemoteViaPort(PORT_NONE),
   mRemoteReceivedPort(PORT_NONE),
   mSocketLock(OsBSem::Q_FIFO, OsBSem::FULL),
   mFirstResendTimeoutMs(SIP_DEFAULT_RTT * 4), // for first transcation time out
   mInUseForWrite(0),
   mWaitingList(NULL),
   mbSharedSocket(FALSE)
{
   touch();

   if (clientSocket)
   {
       clientSocket->getRemoteHostName(&mRemoteHostName);
       clientSocket->getRemoteHostIp(&mRemoteSocketAddress, &mRemoteHostPort);

       OsSysLog::add(FAC_SIP, PRI_INFO,
                     "SipClient[%s]::_ created %s %s socket %d: host '%s' port %d",
                     this->mName.data(),
                     OsSocket::ipProtocolString(mSocketType),
                     mbSharedSocket ? "shared" : "unshared",
                     socket->getSocketDescriptor(),
                     mRemoteHostName.data(), mRemoteHostPort
                     );
   }

}

// Destructor
SipClient::~SipClient()
{
    // Do not delete the event listers they are not subordinate

    // Free the socket
    if(clientSocket)
    {
        // Close the socket to unblock the run method
        // in case it is blocked in a waitForReadyToRead or
        // a read on the clientSocket.  This should also
        // cause the run method to exit.
        if (!mbSharedSocket)
        {
           OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipClient[%s]::~SipClient %p socket %p closing %s socket",
                         this->mName.data(), this,
                         clientSocket, OsSocket::ipProtocolString(mSocketType));
           clientSocket->close();
        }

        // Signal everybody to stop waiting for this SipClient
        signalAllAvailableForWrite();

        // Wait for the task to exit so that it does not
        // reference the socket or other members after they
        // get deleted.
        if(isStarted() || isShuttingDown())
        {
            waitUntilShutDown();
        }

        if (!mbSharedSocket)
        {
            delete clientSocket;
        }
        clientSocket = NULL;
    }
    else if(isStarted() || isShuttingDown())
    {
        // It should not get here but just in case
        waitUntilShutDown();
    }

    if(mWaitingList)
    {
        int numEvents = mWaitingList->entries();
        if(numEvents)
        {
            OsSysLog::add(FAC_SIP, PRI_WARNING, "SipClient[%s]::~SipClient has %d waiting events",
                          this->mName.data(),
                          numEvents);
        }

        delete mWaitingList;
        mWaitingList = NULL;
    }

    // Do not delete the event listers they are not subordinate
}

/* ============================ MANIPULATORS ============================== */

int SipClient::run(void* runArg)
{
    int bytesRead;
    UtlString buffer;
    SipMessage* message = NULL;
    UtlString remoteHostName;
    UtlString viaProtocol;
    UtlString fromIpAddress;
    int fromPort;
    int numFailures = 0;
    UtlBoolean internalShutdown = FALSE;
    UtlBoolean readAMessage = FALSE;
    
    int readBufferSize = HTTP_DEFAULT_SOCKET_BUFFER_SIZE;

    if(mSocketType == OsSocket::UDP)
    {
        readBufferSize = MAX_UDP_PACKET_SIZE;
    }

    while(   !isShuttingDown()
            && !internalShutdown
            && clientSocket
            && clientSocket->isOk())
    {
        if(clientSocket)
        {
#ifdef LOG_TIME
            OsTimeLog eventTimes;
#endif

            message = new SipMessage();

            // Block and wait for the socket to be ready to read
            // clientSocket shouldn't be null
            // in this case some sort of race with the destructor.  This should
            // not actually ever happen.
#ifdef TEST_SOCKET
            OsSysLog::add(FAC_SIP, PRI_DEBUG,
                          "SipClient[%s]::run preparing to waitForReadyToRead readAMessage = %d, "
                          "buffer.length() = %d, clientSocket = %p",
                          this->mName.data(),
                          readAMessage, buffer.length(), clientSocket);
#endif
#ifdef LOG_TIME
                eventTimes.addEvent("waiting");
#endif
            if (clientSocket
                && ((  readAMessage && buffer.length() >= MINIMUM_SIP_MESSAGE_SIZE)
                    || waitForReadyToRead()
                    )
                )
            {
#               ifdef LOG_TIME
                eventTimes.addEvent("locking");
#               endif

                // Lock to prevent multithreaded read or write
                mSocketLock.acquire();

#               ifdef LOG_TIME
                eventTimes.addEvent("locked");
#               endif
                // This second check is in case there is
                // some sort of race with the destructor.  This should
                // not actually ever happen.
                if(clientSocket)
                {
                   if (OsSysLog::willLog(FAC_SIP, PRI_DEBUG))
                   {
                      OsSysLog::add(FAC_SIP, PRI_DEBUG,
                                    "SipClient[%s]::run %p socket %p host: %s "
                                    "sock addr: %s via addr: %s rcv addr: %s "
                                    "sock type: %s read ready %s",
                                    this->mName.data(),
                                    this, clientSocket,
                                    mRemoteHostName.data(),
                                    mRemoteSocketAddress.data(),
                                    mRemoteViaAddress.data(),
                                    mReceivedAddress.data(),
                                    OsSocket::ipProtocolString(clientSocket->getIpProtocol()),
                                    isReadyToRead() ? "READY" : "NOT READY"
                         );
                   }
#                   ifdef LOG_TIME
                    eventTimes.addEvent("reading");
#                   endif
                    bytesRead = message->read(clientSocket, readBufferSize, &buffer);

#                   if 0 // turn on to check socket read problems
                    OsSysLog::add(FAC_SIP, PRI_DEBUG,
                                  "SipClient[%s]::run client %p read %d bytes",
                                  this->mName.data(),
                                  this, bytesRead);
#                   endif

#                   ifdef LOG_TIME
                    eventTimes.addEvent("read");
#                   endif
                }
                else
                {
                   OsSysLog::add(FAC_SIP, PRI_ERR,
                                 "SipClient[%s]::run client %p socket attempt to read NULL",
                                 this->mName.data(),
                                 this);
                   bytesRead = 0;
                }

                mSocketLock.release();

#               ifdef LOG_TIME
                eventTimes.addEvent("released");
#               endif

                message->replaceShortFieldNames();
                message->getSendAddress(&fromIpAddress, &fromPort);
            }
            else
            {
                bytesRead = 0;

                if(clientSocket == NULL)
                {
                    OsSysLog::add(FAC_SIP, PRI_ERR,
                                  "SipClient[%s]::run client %p socket is NULL",
                                  this->mName.data(),
                                  this);
                }
            }

            if(clientSocket // This second check is in case there is
                // some sort of race with the destructor.  This should
                // not actually ever happen.
               && (bytesRead <= 0 || !clientSocket->isOk()))
            {
                numFailures++;
                readAMessage = FALSE;

                if(numFailures > 8 || !clientSocket->isOk())
                {
                    // The socket has gone sour close down the client
                    remoteHostName.remove(0);
                    clientSocket->getRemoteHostName(&remoteHostName);
                    OsSysLog::add(FAC_SIP, PRI_DEBUG,
                                  "SipClient[%s]::run Shutting down client: %s due to failed socket (%d)? bytes: %d isOk: %s",
                                  this->mName.data(),
                                  remoteHostName.data(), clientSocket->getSocketDescriptor(),
                                  bytesRead, (clientSocket->isOk() ? "OK" : "NO")
                                  );

                    clientSocket->close();
                    internalShutdown = TRUE;
                }
            }
            else if(bytesRead > 0)
            {
                eventTimes.addEvent("bytesread");

                numFailures = 0;
                touch();

                if(sipUserAgent)
                {
                    UtlString socketRemoteHost;
                    UtlString lastAddress;
                    UtlString lastProtocol;
                    int lastPort;

                    // Only bother processing if the logs are enabled
                    if (   sipUserAgent->isMessageLoggingEnabled()
                        || OsSysLog::willLog(FAC_SIP_INCOMING, PRI_INFO)
                        )
                    {
                       UtlString logMessage;
                       logMessage.append("Read SIP message:\n");
                       logMessage.append("----Remote Host:");
                       logMessage.append(fromIpAddress);
                       logMessage.append("---- Port: ");
                       char buff[10];
                       sprintf(buff, "%d",
                               !portIsValid(fromPort) ? 5060 : fromPort);
                       logMessage.append(buff);
                       logMessage.append("----\n");

                       logMessage.append(buffer.data(), bytesRead);
                       UtlString messageString;
                       logMessage.append(messageString);
                       logMessage.append("====================END====================\n");

                       sipUserAgent->logMessage(logMessage.data(), logMessage.length());
                       OsSysLog::add(FAC_SIP_INCOMING, PRI_INFO, "%s", logMessage.data());
                    }

                    // Set the date field if not present
                    long epochDate;
                    if(!message->getDateField(&epochDate))
                    {
                        message->setDateField();
                    }

                    message->setSendProtocol(mSocketType);
                    message->setTransportTime(touchedTime);
                    clientSocket->getRemoteHostIp(&socketRemoteHost);

                    // Keep track of where this message came from
                    message->setSendAddress(fromIpAddress.data(), fromPort);
                    
                    // Keep track of the interface on which this message was
                    // received.               
                    message->setLocalIp(clientSocket->getLocalIp());

                    if(mReceivedAddress.isNull())
                    {
                        mReceivedAddress = fromIpAddress;
                        mRemoteReceivedPort = fromPort;
                    }

                    // If this is a request
                    if(!message->isResponse())
                    {
                       int receivedPort;
                       UtlBoolean receivedSet;
                       UtlBoolean maddrSet;
                       UtlBoolean receivedPortSet;

                       // fill in 'received' and 'rport' in top via if needed.
                       message->setReceivedViaParams(fromIpAddress, fromPort);

                       // get the addresses from the topmost via.
                       message->getLastVia(&lastAddress, &lastPort, &lastProtocol,
                                           &receivedPort, &receivedSet, &maddrSet,
                                           &receivedPortSet);

                        if (   (   mSocketType == OsSocket::TCP
                                || mSocketType == OsSocket::SSL_SOCKET
                                )
                            && !receivedPortSet
                            )
                        {
                            // we can use this socket as if it were
                            // connected to the port specified in the
                            // via field
                            mRemoteReceivedPort = lastPort;
                        }

                        // Keep track of the address the other
                        // side said they sent from.  Note, this cannot
                        // be trusted unless this transaction is
                        // authenticated
                        if(mRemoteViaAddress.isNull())
                        {
                            mRemoteViaAddress = lastAddress;
                            mRemoteViaPort = portIsValid(lastPort) ? lastPort : 5060;
                        }
                    }

                    // We read a whole message whether it is a valid one or
                    // not does not matter
                    readAMessage = TRUE;

                    // Check that we have the minimum data to define a transaction
                    UtlString callId;
                    UtlString fromField;
                    UtlString toField;
                    message->getCallIdField(&callId);
                    message->getFromField(&fromField);
                    message->getToField(&toField);

                    if(!(   callId.isNull()
                         || fromField.isNull()
                         || toField.isNull()))
                    {
#ifdef LOG_TIME
                       eventTimes.addEvent("dispatching");
#endif
                        sipUserAgent->dispatch(message);
#ifdef LOG_TIME
                        eventTimes.addEvent("dispatched");
#endif

                        message = NULL; // protect the dispatched message from deletion below
                    }
                    else
                    {
                       // Only bother processing if the logs are enabled
                       if (sipUserAgent->isMessageLoggingEnabled())
                       {
                          UtlString msgBytes;
                                    int msgLen;
                                    message->getBytes(&msgBytes, &msgLen);
                                    msgBytes.insert(0, "Received incomplete message (missing To, From or Call-Id header)\n");
                                    msgBytes.append("++++++++++++++++++++END++++++++++++++++++++\n");
                                    sipUserAgent->logMessage(msgBytes.data(), msgBytes.length());
                       }

                       delete message;
                       message = NULL;
                    }

                } //if sipuseragent

                // Get rid of the consumed stuff in the buffer so it
                // contains only bytes which are part of the next message
                buffer.remove(0, bytesRead);

                if(   mSocketType == OsSocket::UDP
                   && buffer.length()
                   )
                {
                    OsSysLog::add(FAC_SIP, 
                                  // For UDP, this is an error, but not
                                  // for TCP or TLS.
                                  (clientSocket->getIpProtocol() ==
                                   OsSocket::UDP) ? PRI_ERR : PRI_DEBUG,
                                  "SipClient[%s]::run buffer residual bytes: %d\n===>%s<===\n",
                                  this->mName.data(),
                                  buffer.length(), buffer.data());
                }
            } // if bytesRead > 0

#           ifdef LOG_TIME
            {
               UtlString timeString;
               eventTimes.getLogString(timeString);
               OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipClient[%s]::run time log: %s",
                             this->mName.data(),
                             timeString.data());
            }
#           endif
            if(message)
            {
                delete message;
            }
            message = NULL;
        }
        else
        {
           OsSysLog::add(FAC_SIP, PRI_ERR, "SipClient[%s]::run client %p socket is NULL yielding",
                         this->mName.data(),
                         this);
           yield();  // I do not know why this yield is here
        }
    } // while this client is ok

    return(0);
}

// Test whether the socket is ready to read. (Does not block.)
UtlBoolean SipClient::isReadyToRead()
{
   return clientSocket->isReadyToRead(0);
}

// Wait until the socket is ready to read (or has an error).
UtlBoolean SipClient::waitForReadyToRead()
{
   return clientSocket->isReadyToRead(-1);
}

UtlBoolean SipClient::send(SipMessage* message)
{
   UtlBoolean sendOk = FALSE;
   UtlString viaProtocol;

   if (clientSocket)
   {
      if (!clientSocket->isOk())
      {
         clientSocket->reconnect();
#ifdef TEST_SOCKET
         if (clientSocket)
         {
            OsSysLog::add(FAC_SIP, PRI_DEBUG,
                          "SipClient[%s]::send reconnected with socket descriptor %d",
                          mName.data(),
                          clientSocket->getSocketDescriptor());
         }
#endif
      }
      else
      {
#ifdef LOG_TIME
         OsTimeLog eventTimes;
         eventTimes.addEvent("wait to write");
#endif
         // Wait until the socket is ready to write
         if (clientSocket->isReadyToWrite(mFirstResendTimeoutMs))
         {
#ifdef LOG_TIME
            eventTimes.addEvent("wait to lock");
#endif
            // Lock to prevent multitreaded read or write
            mSocketLock.acquire();
#ifdef LOG_TIME
            eventTimes.addEvent("writing");
#endif
            sendOk = message->write(clientSocket);
#ifdef LOG_TIME
            eventTimes.addEvent("releasing");
#endif
            mSocketLock.release();
#ifdef LOG_TIME
            eventTimes.addEvent("released");
            UtlString timeString;
            eventTimes.getLogString(timeString);
            OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipClient[%s]::send time log: %s",
                          this->mName.data(),
                          timeString.data());
#endif

            if (sendOk)
            {
               touch();
            }
         }
         else
         {
            clientSocket->close();
         }
      }
   }

   return (sendOk);
}

UtlBoolean SipClient::sendTo(const SipMessage& message,
                            const char* address,
                            int port)
{
        UtlBoolean sendOk = FALSE;
        UtlString viaProtocol;

    if(clientSocket)
    {
       switch (mSocketType)
       {
       case OsSocket::UDP:
       {
          UtlString buffer;
          int bufferLen;
          int bytesWritten;

          message.getBytes(&buffer, &bufferLen);
          // Wait until the socket is ready to write
          if(clientSocket->isReadyToWrite(mFirstResendTimeoutMs))
          {
             //Lock to prevent multitreaded read or write
             mSocketLock.acquire();
             bytesWritten =
                (dynamic_cast <OsDatagramSocket*> (clientSocket))->
                write(buffer.data(), bufferLen, address,
                      // PORT_NONE means use default.
                      (port == PORT_NONE) ? SIP_PORT : port
                   );
             mSocketLock.release();

             if(bufferLen == bytesWritten)
             {
                sendOk = TRUE;
                touch();
             }
             else
             {
                sendOk = FALSE;
             }
          }
       }
       break;

       case OsSocket::TCP:
       case OsSocket::SSL_SOCKET:
          sendOk = send((SipMessage*) &message) ;
          break;

       default:
          OsSysLog::add(FAC_SIP, PRI_CRIT,
                        "SipClient[%s]::sendTo called for invalid socket type %d",
                        this->mName.data(),
                        mSocketType
                        );
          sendOk = FALSE;
       }
    }
    else
    {
       OsSysLog::add(FAC_SIP, PRI_CRIT,
                     "SipClient[%s]::sendTo called for client without socket",
                     this->mName.data()
                     );
       sendOk = FALSE;
    }

    return(sendOk);
}

void SipClient::notifyWhenAvailableForWrite(OsEvent& availableEvent)
{

    if(mWaitingList == NULL)
    {
        mWaitingList = new UtlSList();
    }

    UtlInt* eventNode = new UtlInt((int)&availableEvent);

    mWaitingList->append(eventNode);
}

void SipClient::signalNextAvailableForWrite()
{
    if(mWaitingList)
    {
        // Remove the first event that is waiting for this transaction
        UtlInt* eventNode = (UtlInt*) mWaitingList->get();

        if(eventNode)
        {
            OsEvent* waitingEvent = (OsEvent*) eventNode->getValue();

            if(waitingEvent)
            {
               // If the other side is done accessing the event
               // and has already signalled it, then we can delete
               // this.  Otherwise the other side must do the delete.
               if(OS_ALREADY_SIGNALED == waitingEvent->signal(1))
               {
                  delete waitingEvent;
                  waitingEvent = NULL;
               }
            }
            delete eventNode;
            eventNode = NULL;
        }
    }
}

void SipClient::signalAllAvailableForWrite()
{
    if(mWaitingList)
    {
        // Remove the first event that is waiting for this transaction
        UtlInt* eventNode = NULL;
        while((eventNode = (UtlInt*) mWaitingList->get()))
        {
            if(eventNode)
            {
                OsEvent* waitingEvent = (OsEvent*) eventNode->getValue();
                if(waitingEvent)
                {
                   // If the other side is done accessing the event
                   // and has already signalled it, then we can delete
                   // this.  Otherwise the other side must do the delete.
                   if(OS_ALREADY_SIGNALED == waitingEvent->signal(1))
                   {
                      delete waitingEvent;
                      waitingEvent = NULL;
                   }
                }
                delete eventNode;
                eventNode = NULL;
            }
        }
    }
}

void SipClient::setSharedSocket(UtlBoolean bShared)
{
    mbSharedSocket = bShared ;
}
/* ============================ ACCESSORS ================================= */

void SipClient::getClientNames(UtlString& clientNames) const
{
    char portString[40];

    // host DNS name
    sprintf(portString, "%d", mRemoteHostPort);
    clientNames = " remote host: ";
    clientNames.append(mRemoteHostName);
    clientNames.append(":");
    clientNames.append(portString);

    // host IP address
    clientNames.append(" remote IP: ");
    clientNames.append(mRemoteSocketAddress);
    clientNames.append(":");
    clientNames.append(portString);

    // via address
    clientNames.append(" remote Via address: ");
    clientNames.append(mRemoteViaAddress);
    clientNames.append(":");
    XmlDecimal(clientNames, mRemoteViaPort);

    // recieved address
    clientNames.append(" received address: ");
    clientNames.append(mReceivedAddress);
    clientNames.append(":");
    XmlDecimal(clientNames, mRemoteReceivedPort);
}

void SipClient::setUserAgent(SipUserAgentBase* sipUA)
{
   sipUserAgent = sipUA;
   //mFirstResendTimeoutMs = (sipUserAgent->getFirstResendTimeout()) * 4;
}

long SipClient::getLastTouchedTime() const
{
   return (touchedTime);
}

void SipClient::touch()
{
   OsTime time;
   OsDateTime::getCurTimeSinceBoot(time);
   touchedTime = time.seconds();
   //OsSysLog::add(FAC_SIP, PRI_DEBUG, "SipClient[%s]::touch client: %p time: %d\n",
   //             this->mName.data(), this, touchedTime);
}

int SipClient::isInUseForWrite(void)
{
    return(mInUseForWrite);
}

/* ============================ INQUIRY =================================== */

UtlBoolean SipClient::isOk()
{
        return(clientSocket->isOk() && !isShuttingDown());
}

UtlBoolean SipClient::isConnectedTo(UtlString& hostName, int hostPort)
{
    UtlBoolean isSame = FALSE;
    int tempHostPort = portIsValid(hostPort) ? hostPort : SIP_PORT; // :TODO: not correct for TLS

#ifdef TEST_SOCKET
    OsSysLog::add(FAC_SIP, PRI_DEBUG,
                  "SipClient[%s]::isConnectedTo hostName = '%s', tempHostPort = %d, mRemoteHostName = '%s', mRemoteHostPort = %d, mRemoteSocketAddress = '%s', mReceivedAddress = '%s', mRemoteViaAddress = '%s'",
                  this->mName.data(),
                  hostName.data(), tempHostPort, mRemoteHostName.data(), mRemoteHostPort, mRemoteSocketAddress.data(), mReceivedAddress.data(), mRemoteViaAddress.data());
#endif

    // If the ports match and the host is the same as either the
    // original name that the socket was constructed with or the
    // name it was resolved to (usually an IP address).
    if (   mRemoteHostPort == tempHostPort
        && (   hostName.compareTo(mRemoteHostName, UtlString::ignoreCase) == 0
            || hostName.compareTo(mRemoteSocketAddress, UtlString::ignoreCase) == 0))
    {
        isSame = TRUE;
    }
    else if (   mRemoteReceivedPort == tempHostPort
             && hostName.compareTo(mReceivedAddress, UtlString::ignoreCase) == 0)
    {
        isSame = TRUE;
    }
    else if (   mRemoteViaPort == tempHostPort
             && hostName.compareTo(mRemoteViaAddress, UtlString::ignoreCase) == 0)
    {
        // Cannot trust what the other side said was their IP address
        // as this is a bad spoofing/denial of service hole
        OsSysLog::add(FAC_SIP, PRI_DEBUG,
                      "SipClient[%s]::isConnectedTo matches %s:%d but is not trusted",
                      this->mName.data(),
                      mRemoteViaAddress.data(), mRemoteViaPort);
    }

    return(isSame);
}

void SipClient::markInUseForWrite()
{
    OsTime time;
    OsDateTime::getCurTimeSinceBoot(time);
    mInUseForWrite = time.seconds();
}

void SipClient::markAvailbleForWrite()
{
    mInUseForWrite = 0;
    signalNextAvailableForWrite();
}

const UtlString& SipClient::getLocalIp()
{
    return clientSocket->getLocalIp();
}


/* //////////////////////////// PROTECTED ///////////////////////////////// */

/* //////////////////////////// PRIVATE /////////////////////////////////// */

/* ============================ FUNCTIONS ================================= */
