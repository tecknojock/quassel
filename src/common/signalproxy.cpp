/***************************************************************************
 *   Copyright (C) 2005-08 by the Quassel Project                          *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "signalproxy.h"

#include <QObject>
#include <QIODevice>
#include <QAbstractSocket>
#include <QHash>
#include <QMultiHash>
#include <QList>
#include <QSet>
#include <QDebug>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QRegExp>
#include <QThread>

#include "syncableobject.h"
#include "util.h"

class SignalRelay: public QObject {

/* Q_OBJECT is not necessary or even allowed, because we implement
   qt_metacall ourselves (and don't use any other features of the meta
   object system)
*/
public:
  SignalRelay(SignalProxy* parent, QObject* source);
  int qt_metacall(QMetaObject::Call _c, int _id, void **_a);

  void attachSignal(int methodId, const QByteArray &func);

  void setSynchronize(bool);
  bool synchronize() const;
  
  int sigCount() const;
  
private:
  bool isSyncMethod(int i);
  
  SignalProxy* proxy;
  QObject* caller;
  QMultiHash<int, QByteArray> sigNames;
  bool _sync;
};

SignalRelay::SignalRelay(SignalProxy* parent, QObject* source)
  : QObject(parent),
    proxy(parent),
    caller(source),
    _sync(false)
{
  QObject::connect(source, SIGNAL(destroyed()), parent, SLOT(detachSender()));
}

int SignalRelay::qt_metacall(QMetaObject::Call _c, int _id, void **_a) {
  _id = QObject::qt_metacall(_c, _id, _a);
  if(_id < 0)
    return _id;
  if(_c == QMetaObject::InvokeMetaMethod) {
    if(sigNames.contains(_id) || synchronize()) {
      const QList<int> &argTypes = proxy->argTypes(caller, _id);
      QVariantList params;
      int n = argTypes.size();
      for(int i=0; i<n; i++) {
	if(argTypes[i] == 0) {
	  qWarning() << "SignalRelay::qt_metacall(): received invalid data for argument number" << i << "of signal" << QString("%1::%2").arg(caller->metaObject()->className()).arg(caller->metaObject()->method(_id).signature());
	  qWarning() << "                            - make sure all your data types are known by the Qt MetaSystem";
	  return _id;
	}
	params.append(QVariant(argTypes[i], _a[i+1]));
      }
      QMultiHash<int, QByteArray>::const_iterator funcIter = sigNames.constFind(_id);
      while(funcIter != sigNames.constEnd() && funcIter.key() == _id) {
        proxy->dispatchSignal(SignalProxy::RpcCall, QVariantList() << funcIter.value() << params);
        funcIter++;
      }
      
      // dispatch Sync Signal if necessary
      QByteArray signature(caller->metaObject()->method(_id).signature());
      SyncableObject *syncObject = qobject_cast<SyncableObject *>(caller);
      if(synchronize() && proxy->syncMap(syncObject).contains(signature)) {
	 //qDebug() << "__SYNC__ >>>"
	 //	 << caller->metaObject()->className()
	 //	 << caller->objectName()
	 //	 << signature
	 //	 << params;
	// params.prepend(QVariant(_id));
	params.prepend(signature);
	params.prepend(syncObject->objectName());
	params.prepend(syncObject->syncMetaObject()->className());
	proxy->dispatchSignal(SignalProxy::Sync, params);
      }
    }
    _id -= QObject::staticMetaObject.methodCount();
  }
  return _id;
}

void SignalRelay::setSynchronize(bool sync) {
  SyncableObject *syncObject = qobject_cast<SyncableObject *>(caller);
  if(!syncObject)
    return;

  const QMetaObject *meta = syncObject->syncMetaObject();
  if(!_sync && sync) {
    // enable Sync
    for(int i = 0; i < meta->methodCount(); i++ ) {
      if(isSyncMethod(i))
	QMetaObject::connect(caller, i, this, QObject::staticMetaObject.methodCount() + i);
    }
  } else if (_sync && !sync) {
    // disable Sync
    for(int i = 0; i < meta->methodCount(); i++ ) {
      if(isSyncMethod(i))
	QMetaObject::disconnect(caller, i, this, QObject::staticMetaObject.methodCount() + i);
    }
  }
  _sync = sync;
}

bool SignalRelay::isSyncMethod(int i) {
  SyncableObject *syncObject = qobject_cast<SyncableObject *>(caller);
  if(!syncObject)
    return false;

  QByteArray signature = syncObject->syncMetaObject()->method(i).signature();
  if(!proxy->syncMap(syncObject).contains(signature))
    return false;
  
  if(proxy->proxyMode() == SignalProxy::Server && !signature.contains("Requested"))
    return true;

  if(proxy->proxyMode() == SignalProxy::Client && signature.contains("Requested"))
    return true;

  return false;
}

bool SignalRelay::synchronize() const {
  return _sync;
}

int SignalRelay::sigCount() const {
  // only for debuging purpose
  return sigNames.count();
}

void SignalRelay::attachSignal(int methodId, const QByteArray &func) {
  // we ride without safetybelts here... all checking for valid method etc pp has to be done by the caller
  // all connected methodIds are offset by the standard methodCount of QObject
  if(!sigNames.contains(methodId))
    QMetaObject::connect(caller, methodId, this, QObject::staticMetaObject.methodCount() + methodId);

  QByteArray fn;
  if(!func.isEmpty()) {
    fn = QMetaObject::normalizedSignature(func);
  } else {
    fn = QByteArray("2") + caller->metaObject()->method(methodId).signature();
  }
  sigNames.insert(methodId, fn);
}
// ====================
// END SIGNALRELAY
// ====================


// ====================
//  SignalProxy
// ====================
SignalProxy::SignalProxy(QObject* parent)
  : QObject(parent)
{
  setProxyMode(Client);
}

SignalProxy::SignalProxy(ProxyMode mode, QObject* parent)
  : QObject(parent)
{
  setProxyMode(mode);
}

SignalProxy::SignalProxy(ProxyMode mode, QIODevice* device, QObject* parent)
  : QObject(parent)
{
  setProxyMode(mode);
  addPeer(device);
} 

SignalProxy::~SignalProxy() {
  QList<QObject*> senders = _relayHash.keys();
  foreach(QObject* sender, senders)
    detachObject(sender);

  // close peer connections
  foreach(QIODevice *device, _peerByteCount.keys()) {
    device->close();
    delete device;
  }
}

void SignalProxy::setProxyMode(ProxyMode mode) {
  foreach(QIODevice* peer, _peerByteCount.keys()) {
    if(peer->isOpen()) {
      qWarning() << "SignalProxy: Cannot change proxy mode while connected";
      return;
    }
  }
  _proxyMode = mode;
  if(mode == Server)
    initServer();
  else
    initClient();
}

SignalProxy::ProxyMode SignalProxy::proxyMode() const {
  return _proxyMode;
}

void SignalProxy::initServer() {
  disconnect(&_heartBeatTimer, 0, this, 0);
  _heartBeatTimer.stop();
}

void SignalProxy::initClient() {
  attachSlot("__objectRenamed__", this, SLOT(objectRenamed(QByteArray, QString, QString)));
  connect(&_heartBeatTimer, SIGNAL(timeout()),
	  this, SLOT(sendHeartBeat()));
  _heartBeatTimer.start(60 * 1000); // msecs: one beep per minute
}

bool SignalProxy::addPeer(QIODevice* iodev) {
  if(!iodev)
    return false;
  
  if(_peerByteCount.contains(iodev))
    return true;

  if(proxyMode() == Client && !_peerByteCount.isEmpty()) {
    qWarning("SignalProxy: only one peer allowed in client mode!");
    return false;
  }

  if(!iodev->isOpen())
    qWarning("SignalProxy::the device you passed is not open!");

  connect(iodev, SIGNAL(disconnected()), this, SLOT(removePeerBySender()));
  connect(iodev, SIGNAL(readyRead()), this, SLOT(dataAvailable()));

  QAbstractSocket* sock  = qobject_cast<QAbstractSocket*>(iodev);
  if(sock) {
    connect(sock, SIGNAL(disconnected()), this, SLOT(removePeerBySender()));
  }

  _peerByteCount[iodev] = 0;

  if(_peerByteCount.count() == 1)
    emit connected();

  return true;
}

void SignalProxy::removePeer(QIODevice* iodev) {
  if(_peerByteCount.isEmpty()) {
    qWarning() << "SignalProxy::removePeer(): No peers in use!";
    return;
  }

  if(proxyMode() == Server && !iodev) {
    // disconnect all
    QList<QIODevice *> peers = _peerByteCount.keys();
    foreach(QIODevice *peer, peers)
      removePeer(peer);
  }

  if(proxyMode() != Server && !iodev)
    iodev = _peerByteCount.keys().first();

  Q_ASSERT(iodev);

  if(!_peerByteCount.contains(iodev)) {
    qWarning() << "SignalProxy: unknown QIODevice" << iodev;
    return;
  }

  _peerByteCount.remove(iodev);

  disconnect(iodev, 0, this, 0);
  emit peerRemoved(iodev);

  if(_peerByteCount.isEmpty())
    emit disconnected();
}

void SignalProxy::removePeerBySender() {
  // OK we're brutal here... but since it's a private slot we know what we've got connected to it...
  QIODevice *ioDev = (QIODevice *)(sender());
  removePeer(ioDev);
  qDebug() << "Client disconnected.";
}

void SignalProxy::objectRenamed(const QString &newname, const QString &oldname) {
  SyncableObject *syncObject = qobject_cast<SyncableObject *>(sender());
  const QMetaObject *meta = syncObject->metaObject();
  const QByteArray className(meta->className());
  objectRenamed(className, newname, oldname);

  if(proxyMode() == Client)
    return;
  
  QVariantList params;
  params << "__objectRenamed__" << className << newname << oldname;
  dispatchSignal(RpcCall, params);
}

void SignalProxy::objectRenamed(const QByteArray &classname, const QString &newname, const QString &oldname) {
  if(_syncSlave.contains(classname) && _syncSlave[classname].contains(oldname) && oldname != newname) {
    SyncableObject *obj = _syncSlave[classname][newname] = _syncSlave[classname].take(oldname);
    requestInit(obj);
  }
}

void SignalProxy::setArgTypes(QObject* obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  QList<QByteArray> p = meta->method(methodId).parameterTypes();
  QList<int> argTypes;
  int ct = p.count();
  for(int i=0; i<ct; i++)
    argTypes.append(QMetaType::type(p.value(i)));

  Q_ASSERT(!_classInfo[meta]->argTypes.contains(methodId));
  _classInfo[meta]->argTypes[methodId] = argTypes;
}

const QList<int> &SignalProxy::argTypes(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  Q_ASSERT(_classInfo.contains(meta));
  if(!_classInfo[meta]->argTypes.contains(methodId))
    setArgTypes(obj, methodId);
  return _classInfo[meta]->argTypes[methodId];
}

void SignalProxy::setReturnType(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  int returnType = QMetaType::type(meta->method(methodId).typeName());

  Q_ASSERT(!_classInfo[meta]->returnType.contains(methodId));
  _classInfo[meta]->returnType[methodId] = returnType;
}

const int &SignalProxy::returnType(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  Q_ASSERT(_classInfo.contains(meta));
  if(!_classInfo[meta]->returnType.contains(methodId))
    setReturnType(obj, methodId);
  return _classInfo[meta]->returnType[methodId];
}

void SignalProxy::setMinArgCount(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  QString signature(meta->method(methodId).signature());
  int minCount = meta->method(methodId).parameterTypes().count() - signature.count("=");
  Q_ASSERT(!_classInfo[meta]->minArgCount.contains(methodId));
  _classInfo[meta]->minArgCount[methodId] = minCount;
}

const int &SignalProxy::minArgCount(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  Q_ASSERT(_classInfo.contains(meta));
  if(!_classInfo[meta]->minArgCount.contains(methodId))
    setMinArgCount(obj, methodId);
  return _classInfo[meta]->minArgCount[methodId];
}

void SignalProxy::setMethodName(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  QByteArray method(::methodName(meta->method(methodId)));
  Q_ASSERT(!_classInfo[meta]->methodNames.contains(methodId));
  _classInfo[meta]->methodNames[methodId] = method;
}

const QByteArray &SignalProxy::methodName(QObject *obj, int methodId) {
  const QMetaObject *meta = metaObject(obj);
  Q_ASSERT(_classInfo.contains(meta));
  if(!_classInfo[meta]->methodNames.contains(methodId))
    setMethodName(obj, methodId);
  return _classInfo[meta]->methodNames[methodId];
}


void SignalProxy::setSyncMap(SyncableObject *obj) {
  const QMetaObject *meta = obj->syncMetaObject();
  QHash<QByteArray, int> syncMap;
  
  QList<int> slotIndexes;
  for(int i = 0; i < meta->methodCount(); i++) {
    if(meta->method(i).methodType() == QMetaMethod::Slot)
      slotIndexes << i;
  }

  // we're faking sync pairs for sync replies
  QByteArray slotSignature;
  foreach(int slotIdx, slotIndexes) {
    slotSignature = QByteArray(meta->method(slotIdx).signature());
    if(!slotSignature.startsWith("receive"))
      continue;
    syncMap[slotSignature] = slotIdx;
  }

  QMetaMethod signal, slot;
  int matchIdx;
  for(int signalIdx = 0; signalIdx < meta->methodCount(); signalIdx++) {
    signal = meta->method(signalIdx);
    if(signal.methodType() != QMetaMethod::Signal)
      continue;

    matchIdx = -1;
    foreach(int slotIdx, slotIndexes) {
      slot = meta->method(slotIdx);
      if(methodsMatch(signal, slot)) {
	matchIdx = slotIdx;
	break;
      }
    }
    if(matchIdx != -1) {
      slotIndexes.removeAt(slotIndexes.indexOf(matchIdx));
      syncMap[QByteArray(signal.signature())] = matchIdx;
    }
  }

  Q_ASSERT(_classInfo[meta]->syncMap.isEmpty());
  _classInfo[meta]->syncMap = syncMap;
}

const QHash<QByteArray,int> &SignalProxy::syncMap(SyncableObject *obj) {
  const QMetaObject *meta = obj->syncMetaObject();
  Q_ASSERT(_classInfo.contains(meta));
  if(_classInfo[meta]->syncMap.isEmpty())
    setSyncMap(obj);
  return _classInfo[meta]->syncMap;
}

void SignalProxy::setReceiveMap(SyncableObject *obj) {
  const QMetaObject *meta = obj->syncMetaObject();
  Q_ASSERT(_classInfo.contains(meta));

  QHash<int, int> receiveMap;

  QMetaMethod requestSlot;
  QByteArray returnTypeName;
  QByteArray signature;
  QByteArray methodName;
  QByteArray params;
  int paramsPos;
  int receiverId;
  const int methodCount = meta->methodCount();
  for(int i = 0; i < methodCount; i++) {
    requestSlot = meta->method(i);
    if(requestSlot.methodType() != QMetaMethod::Slot)
      continue;

    returnTypeName = requestSlot.typeName();
    if(QMetaType::Void == (QMetaType::Type)returnType(obj, i))
      continue;

    signature = QByteArray(requestSlot.signature());
    if(!signature.startsWith("request"))
      continue;
    
    paramsPos = signature.indexOf('(');
    if(paramsPos == -1)
      continue;

    methodName = signature.left(paramsPos);
    params = signature.mid(paramsPos);

    methodName = methodName.replace("request", "receive");
    params = params.left(params.count() - 1) + ", " + returnTypeName + ")";

    signature = QMetaObject::normalizedSignature(methodName + params);
    receiverId = meta->indexOfSlot(signature);

    if(receiverId == -1) {
      signature = QMetaObject::normalizedSignature(methodName + "(" + returnTypeName + ")");
      receiverId = meta->indexOfSlot(signature);
    }

    if(receiverId != -1)
      receiveMap[i] = receiverId;
  }
  _classInfo[meta]->receiveMap = receiveMap;  
}

const QHash<int, int> &SignalProxy::receiveMap(SyncableObject *obj) {
  const QMetaObject *meta = obj->syncMetaObject();
  Q_ASSERT(_classInfo.contains(meta));
  if(_classInfo[meta]->receiveMap.isEmpty())
    setReceiveMap(obj);
  return _classInfo[meta]->receiveMap;
}

void SignalProxy::setUpdatedRemotelyId(SyncableObject *obj) {
  const QMetaObject *meta = obj->syncMetaObject();
  Q_ASSERT(_classInfo.contains(meta));
  _classInfo[meta]->updatedRemotelyId = meta->indexOfSignal("updatedRemotely()");
}

int SignalProxy::updatedRemotelyId(SyncableObject *obj) {
  Q_ASSERT(_classInfo.contains(obj->syncMetaObject()));
  return _classInfo[obj->syncMetaObject()]->updatedRemotelyId;
}

const QMetaObject *SignalProxy::metaObject(QObject *obj) {
  if(SyncableObject *syncObject = qobject_cast<SyncableObject *>(obj))
    return syncObject->syncMetaObject();
  else
    return obj->metaObject();
}

void SignalProxy::createClassInfo(QObject *obj) {
  const QMetaObject *meta = metaObject(obj);
  if(_classInfo.contains(meta))
    return;

  ClassInfo *classInfo = new ClassInfo();
  _classInfo[meta] = classInfo;
}

bool SignalProxy::attachSignal(QObject* sender, const char* signal, const QByteArray& sigName) {
  const QMetaObject* meta = metaObject(sender);
  QByteArray sig(meta->normalizedSignature(signal).mid(1));
  int methodId = meta->indexOfMethod(sig.constData());
  if(methodId == -1 || meta->method(methodId).methodType() != QMetaMethod::Signal) {
    qWarning() << "SignalProxy::attachSignal(): No such signal" << signal;
    return false;
  }

  createClassInfo(sender);

  SignalRelay* relay;
  if(_relayHash.contains(sender))
    relay = _relayHash[sender];
  else
    relay = _relayHash[sender] = new SignalRelay(this, sender);

  relay->attachSignal(methodId, sigName);

  return true;
}


bool SignalProxy::attachSlot(const QByteArray& sigName, QObject* recv, const char* slot) {
  const QMetaObject* meta = recv->metaObject();
  int methodId = meta->indexOfMethod(meta->normalizedSignature(slot).mid(1));
  if(methodId == -1 || meta->method(methodId).methodType() == QMetaMethod::Method) {
    qWarning() << "SignalProxy::attachSlot(): No such slot" << slot;
    return false;
  }

  createClassInfo(recv);

  QByteArray funcName = QMetaObject::normalizedSignature(sigName.constData());
  _attachedSlots.insert(funcName, qMakePair(recv, methodId));

  QObject::disconnect(recv, SIGNAL(destroyed()), this, SLOT(detachSender()));
  QObject::connect(recv, SIGNAL(destroyed()), this, SLOT(detachSender()));
  return true;
}

void SignalProxy::synchronize(SyncableObject *obj) {
  createClassInfo(obj);
  setUpdatedRemotelyId(obj);
  
  // attaching all the Signals
  SignalRelay* relay;
  if(_relayHash.contains(obj))
    relay = _relayHash[obj];
  else
    relay = _relayHash[obj] = new SignalRelay(this, obj);

  relay->setSynchronize(true);

  // attaching as slave to receive sync Calls
  QByteArray className(obj->syncMetaObject()->className());
  _syncSlave[className][obj->objectName()] = obj;

  if(proxyMode() == Server) {
    connect(obj, SIGNAL(objectRenamed(QString, QString)), this, SLOT(objectRenamed(QString, QString)));
    setInitialized(obj);
  } else {
    requestInit(obj);
  }
}

void SignalProxy::setInitialized(SyncableObject *obj) {
  obj->setInitialized();
  emit objectInitialized(obj);
}

bool SignalProxy::isInitialized(SyncableObject *obj) const {
  return obj->isInitialized();
}

void SignalProxy::requestInit(SyncableObject *obj) {
  if(proxyMode() == Server || isInitialized(obj))
    return;

  QVariantList params;
  params << obj->syncMetaObject()->className()
	 << obj->objectName();
  dispatchSignal(InitRequest, params);
}

void SignalProxy::detachSender() {
  detachObject(sender());
}

void SignalProxy::detachObject(QObject* obj) {
  detachSignals(obj);
  detachSlots(obj);
  stopSync(static_cast<SyncableObject *>(obj));
}

void SignalProxy::detachSignals(QObject* sender) {
  if(!_relayHash.contains(sender))
    return;
  _relayHash.take(sender)->deleteLater();
}

void SignalProxy::detachSlots(QObject* receiver) {
  SlotHash::iterator slotIter = _attachedSlots.begin();
  while(slotIter != _attachedSlots.end()) {
    if(slotIter.value().first == receiver) {
      slotIter = _attachedSlots.erase(slotIter);
    } else
      slotIter++;
  }
}

void SignalProxy::stopSync(SyncableObject* obj) {
  if(_relayHash.contains(obj))
    _relayHash[obj]->setSynchronize(false);

  // we can't use a className here, since it might be effed up, if we receive the call as a result of a decon
  // gladly the objectName() is still valid. So we have only to iterate over the classes not each instance! *sigh*
  QHash<QByteArray, ObjectId>::iterator classIter = _syncSlave.begin();
  while(classIter != _syncSlave.end()) {
    if(classIter->contains(obj->objectName()) && classIter.value()[obj->objectName()] == obj) {
      classIter->remove(obj->objectName());
      break;
    }
    classIter++;
  }
}

void SignalProxy::dispatchSignal(QIODevice *receiver, const RequestType &requestType, const QVariantList &params) {
  QVariantList packedFunc;
  packedFunc << (qint16)requestType;
  packedFunc << params;
  writeDataToDevice(receiver, QVariant(packedFunc));
}

void SignalProxy::dispatchSignal(const RequestType &requestType, const QVariantList &params) {
  // yes I know we have a little code duplication here... it's for the sake of performance
  QVariantList packedFunc;
  packedFunc << (qint16)requestType;
  packedFunc << params;
  foreach(QIODevice* dev, _peerByteCount.keys())
    writeDataToDevice(dev, QVariant(packedFunc));
}

void SignalProxy::receivePeerSignal(QIODevice *sender, const QVariant &packedFunc) {
  QVariantList params(packedFunc.toList());

  if(params.isEmpty()) {
    qWarning() << "SignalProxy::receivePeerSignal(): received incompatible Data:" << packedFunc;
    return;
  }
  
  int callType = params.takeFirst().value<int>();

  switch(callType) {
  case RpcCall:
    if(params.empty()) {
      qWarning() << "SignalProxy::receivePeerSignal(): received empty RPC-Call";
      return;
    } else {
      return handleSignal(params.takeFirst().toByteArray(), params);
    }
  case Sync:
    return handleSync(sender, params);
  case InitRequest:
    return handleInitRequest(sender, params);
  case InitData:
    return handleInitData(sender, params);
  case HeartBeat:
    return;
  default:
    qWarning() << "SignalProxy::receivePeerSignal(): received undefined CallType" << callType << params;
    return;
  }
}

void SignalProxy::handleSync(QIODevice *sender, QVariantList params) {
  if(params.count() < 3) {
    qWarning() << "received invalid Sync call" << params;
    return;
  }
  
  QByteArray className = params.takeFirst().toByteArray();
  QString objectName = params.takeFirst().toString();
  QByteArray signal = params.takeFirst().toByteArray();

  if(!_syncSlave.contains(className) || !_syncSlave[className].contains(objectName)) {
    qWarning() << QString("no registered receiver for sync call: %1::%2 (objectName=\"%3\"). Params are:").arg(QString(className)).arg(QString(signal)).arg(objectName)
	       << params;
    return;
  }

  SyncableObject *receiver = _syncSlave[className][objectName];
  if(!syncMap(receiver).contains(signal)) {
    qWarning() << QString("no matching slot for sync call: %1::%2 (objectName=\"%3\"). Params are:").arg(QString(className)).arg(QString(signal)).arg(objectName)
	       << params;
    return;
  }

  int slotId = syncMap(receiver)[signal];

  QVariant returnValue((QVariant::Type)returnType(receiver, slotId));
  if(!invokeSlot(receiver, slotId, params, returnValue)) {
    qWarning("SignalProxy::handleSync(): invokeMethod for \"%s\" failed ", methodName(receiver, slotId).constData());
    return;
  }

  if(returnValue.type() != QVariant::Invalid && receiveMap(receiver).contains(slotId)) {
    int receiverId = receiveMap(receiver)[slotId];
    QVariantList returnParams;
    returnParams << className
		 << objectName
		 << QByteArray(receiver->metaObject()->method(receiverId).signature());
    if(argTypes(receiver, receiverId).count() > 1)
      returnParams << params;
    returnParams << returnValue;
    dispatchSignal(sender, Sync, returnParams);
  }
  
  // send emit update signal
  invokeSlot(receiver, updatedRemotelyId(receiver));
}

void SignalProxy::handleInitRequest(QIODevice *sender, const QVariantList &params) {
  if(params.count() != 2) {
    qWarning() << "SignalProxy::handleInitRequest() received initRequest with invalid param Count:"
	       << params;
    return;
  }
  
  QByteArray className(params[0].toByteArray());
  QString objectName(params[1].toString());
  
  if(!_syncSlave.contains(className)) {
    qWarning() << "SignalProxy::handleInitRequest() received initRequest for unregistered Class:"
	       << className;
    return;
  }

  if(!_syncSlave[className].contains(objectName)) {
    qWarning() << "SignalProxy::handleInitRequest() received initRequest for unregistered Object:"
	       << className << objectName;
    return;
  }
  
  SyncableObject *obj = _syncSlave[className][objectName];

  QVariantList params_;
  params_ << className
	  << objectName
	  << initData(obj);

  dispatchSignal(sender, InitData, params_);
}

void SignalProxy::handleInitData(QIODevice *sender, const QVariantList &params) {
  Q_UNUSED(sender)
  if(params.count() != 3) {
    qWarning() << "SignalProxy::handleInitData() received initData with invalid param Count:"
	       << params;
    return;
  }
  
  QByteArray className(params[0].toByteArray());
  QString objectName(params[1].toString());
  QVariantMap propertyMap(params[2].toMap());

  if(!_syncSlave.contains(className)) {
    qWarning() << "SignalProxy::handleInitData() received initData for unregistered Class:"
	       << className;
    return;
  }

  if(!_syncSlave[className].contains(objectName)) {
    qWarning() << "SignalProxy::handleInitData() received initData for unregistered Object:"
	       << className << objectName;
    return;
  }

  SyncableObject *obj = _syncSlave[className][objectName];
  setInitData(obj, propertyMap);
}

void SignalProxy::handleSignal(const QByteArray &funcName, const QVariantList &params) {
  QObject* receiver;
  int methodId;
  SlotHash::const_iterator slot = _attachedSlots.constFind(funcName);
  while(slot != _attachedSlots.constEnd() && slot.key() == funcName) {
    receiver = (*slot).first;
    methodId = (*slot).second;
    if(!invokeSlot(receiver, methodId, params))
      qWarning("SignalProxy::handleSignal(): invokeMethod for \"%s\" failed ", methodName(receiver, methodId).constData());
    slot++;
  }
}

bool SignalProxy::invokeSlot(QObject *receiver, int methodId, const QVariantList &params, QVariant &returnValue) {
  const QList<int> args = argTypes(receiver, methodId);
  const int numArgs = params.count() < args.count()
    ? params.count()
    : args.count();

  if(minArgCount(receiver, methodId) > params.count()) {
      qWarning() << "SignalProxy::invokeSlot(): not enough params to invoke" << methodName(receiver, methodId);
      return false;
  }

  void *_a[] = {0,              // return type... 
		0, 0, 0, 0 , 0, // and 10 args - that's the max size qt can handle with signals and slots
		0, 0, 0, 0 , 0};

  // check for argument compatibility and build params array
  for(int i = 0; i < numArgs; i++) {
    if(!params[i].isValid()) {
      qWarning() << "SignalProxy::invokeSlot(): received invalid data for argument number" << i << "of method" << QString("%1::%2()").arg(receiver->metaObject()->className()).arg(receiver->metaObject()->method(methodId).signature());
      qWarning() << "                            - make sure all your data types are known by the Qt MetaSystem";
      return false;
    }
    if(args[i] != QMetaType::type(params[i].typeName())) {
      qWarning() << "SignalProxy::invokeSlot(): incompatible param types to invoke" << methodName(receiver, methodId);
      return false;
    }
    _a[i+1] = const_cast<void *>(params[i].constData());
  }

  if(returnValue.type() != QVariant::Invalid)
    _a[0] = const_cast<void *>(returnValue.constData());
    
  Qt::ConnectionType type = QThread::currentThread() == receiver->thread()
    ? Qt::DirectConnection
    : Qt::QueuedConnection;

  if (type == Qt::DirectConnection) {
    return receiver->qt_metacall(QMetaObject::InvokeMetaMethod, methodId, _a) < 0;
  } else {
    qWarning() << "Queued Connections are not implemented yet";
    // note to self: qmetaobject.cpp:990 ff
    return false;
  }
  
}

bool SignalProxy::invokeSlot(QObject *receiver, int methodId, const QVariantList &params) {
  QVariant ret;
  return invokeSlot(receiver, methodId, params, ret);
}

void SignalProxy::dataAvailable() {
  // yet again. it's a private slot. no need for checks.
  QIODevice* ioDev = qobject_cast<QIODevice* >(sender());
  QVariant var;
  while(readDataFromDevice(ioDev, _peerByteCount[ioDev], var))
    receivePeerSignal(ioDev, var);
}

void SignalProxy::writeDataToDevice(QIODevice *dev, const QVariant &item) {
  QAbstractSocket* sock  = qobject_cast<QAbstractSocket*>(dev);
  if(!dev->isOpen() || (sock && sock->state()!=QAbstractSocket::ConnectedState)) {
    qWarning("SignalProxy: Can't call on a closed device");
    return;
  }
  QByteArray block;
  QDataStream out(&block, QIODevice::WriteOnly);
  out.setVersion(QDataStream::Qt_4_2);
  out << (quint32)0 << item;
  out.device()->seek(0);
  out << (quint32)(block.size() - sizeof(quint32));
  dev->write(block);
}

bool SignalProxy::readDataFromDevice(QIODevice *dev, quint32 &blockSize, QVariant &item) {
  QDataStream in(dev);
  in.setVersion(QDataStream::Qt_4_2);

  if(blockSize == 0) {
    if(dev->bytesAvailable() < (int)sizeof(quint32)) return false;
    in >> blockSize;
  }

  if(dev->bytesAvailable() < blockSize)
    return false;
  in >> item;
  blockSize = 0;
  return true;
}

bool SignalProxy::methodsMatch(const QMetaMethod &signal, const QMetaMethod &slot) const {
  // if we don't even have the same basename it's a sure NO
  if(methodBaseName(signal) != methodBaseName(slot))
    return false;

  // are the signatures compatible?
  if(!QObject::staticMetaObject.checkConnectArgs(signal.signature(), slot.signature()))
    return false;

  // we take an educated guess if the signals and slots match
  QString signalsuffix = ::methodName(signal).mid(QString(::methodName(signal)).lastIndexOf(QRegExp("[A-Z]"))).toLower();
  QString slotprefix = ::methodName(slot).left(QString(::methodName(slot)).indexOf(QRegExp("[A-Z]"))).toLower();

  uint sizediff = qAbs(slotprefix.size() - signalsuffix.size());
  int ratio = editingDistance(slotprefix, signalsuffix) - sizediff;
  return (ratio < 2);
}

QString SignalProxy::methodBaseName(const QMetaMethod &method) {
  QString methodname = QString(method.signature()).section("(", 0, 0);

  // determine where we have to chop:
  int upperCharPos;
  if(method.methodType() == QMetaMethod::Slot) {
    // we take evertyhing from the first uppercase char if it's slot
    upperCharPos = methodname.indexOf(QRegExp("[A-Z]"));
    if(upperCharPos == -1)
      return QString();
    methodname = methodname.mid(upperCharPos);
  } else {
    // and if it's a signal we discard everything from the last uppercase char
    upperCharPos = methodname.lastIndexOf(QRegExp("[A-Z]"));
    if(upperCharPos == -1)
      return QString();
    methodname = methodname.left(upperCharPos);
  }

  methodname[0] = methodname[0].toUpper();

  return methodname;
}

QVariantMap SignalProxy::initData(SyncableObject *obj) const {
  return obj->toVariantMap();
}

void SignalProxy::setInitData(SyncableObject *obj, const QVariantMap &properties) {
  if(isInitialized(obj))
    return;
  obj->fromVariantMap(properties);
  setInitialized(obj);
  invokeSlot(obj, updatedRemotelyId(obj));
}

void SignalProxy::sendHeartBeat() {
  dispatchSignal(SignalProxy::HeartBeat, QVariantList());
}

void SignalProxy::dumpProxyStats() {
  QString mode;
  if(proxyMode() == Server)
    mode = "Server";
  else
    mode = "Client";

  int sigCount = 0;
  foreach(SignalRelay *relay, _relayHash.values())
    sigCount += relay->sigCount();

  int slaveCount = 0;
  foreach(ObjectId oid, _syncSlave.values())
    slaveCount += oid.count();
  
  qDebug() << this;
  qDebug() << "              Proxy Mode:" << mode;
  qDebug() << "attached sending Objects:" << _relayHash.count();
  qDebug() << "       number of Signals:" << sigCount;
  qDebug() << "          attached Slots:" << _attachedSlots.count();
  qDebug() << " number of synced Slaves:" << slaveCount;
  qDebug() << "number of Classes cached:" << _classInfo.count();
}

void SignalProxy::dumpSyncMap(SyncableObject *object) {
  const QMetaObject *meta = object->metaObject();
  qDebug() << "SignalProxy: SyncMap for Class" << meta->className();

  QHash<QByteArray, int> syncMap_ = syncMap(object);
  QHash<QByteArray, int>::const_iterator iter = syncMap_.constBegin();
  while(iter != syncMap_.constEnd()) {
    qDebug() << iter.key() << "-->" << iter.value() << meta->method(iter.value()).signature();    
    iter++;
  }
//   QHash<int, int> syncMap_ = syncMap(object);
//   QHash<int, int>::const_iterator iter = syncMap_.constBegin();
//   while(iter != syncMap_.constEnd()) {
//     qDebug() << iter.key() << meta->method(iter.key()).signature() << "-->" << iter.value() << meta->method(iter.value()).signature();    
//     iter++;
//   }
}
