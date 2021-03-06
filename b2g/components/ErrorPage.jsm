/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

this.EXPORTED_SYMBOLS = ['ErrorPage'];

const Cu = Components.utils;
const Cc = Components.classes;
const Ci = Components.interfaces;
const kErrorPageFrameScript = 'chrome://browser/content/ErrorPage.js';

Cu.import('resource://gre/modules/Services.jsm');
Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyGetter(this, "CertOverrideService", function () {
  return Cc["@mozilla.org/security/certoverride;1"]
         .getService(Ci.nsICertOverrideService);
});

/**
 * A class to add exceptions to override SSL certificate problems.
 * The functionality itself is borrowed from exceptionDialog.js.
 */
function SSLExceptions(aCallback, aUri, aWindow) {
  this._finishCallback = aCallback;
  this._uri = aUri;
  this._window = aWindow;
};

SSLExceptions.prototype = {
  _finishCallback: null,
  _window: null,
  _uri: null,
  _temporary: null,
  _sslStatus: null,

  getInterface: function SSLE_getInterface(aIID) {
    return this.QueryInterface(aIID);
  },

  QueryInterface: XPCOMUtils.generateQI([Ci.nsIBadCertListener2]),

  /**
   * To collect the SSL status we intercept the certificate error here
   * and store the status for later use.
   */
  notifyCertProblem: function SSLE_notifyCertProblem(aSocketInfo,
                                                     aSslStatus,
                                                     aTargetHost) {
    this._sslStatus = aSslStatus.QueryInterface(Ci.nsISSLStatus);
    Services.tm.currentThread.dispatch({
      run: this._addOverride.bind(this)
    }, Ci.nsIThread.DISPATCH_NORMAL);
    return true; // suppress error UI
  },

  /**
   * Attempt to download the certificate for the location specified to get
   * the SSLState for the certificate and the errors.
   */
  _checkCert: function SSLE_checkCert() {
    this._sslStatus = null;
    if (!this._uri) {
      return;
    }
    let req = new this._window.XMLHttpRequest();
    try {
      req.open("GET", this._uri.prePath, true);
      req.channel.notificationCallbacks = this;
      let xhrHandler = (function() {
        req.removeEventListener("load", xhrHandler);
        req.removeEventListener("error", xhrHandler);
        if (!this._sslStatus) {
          // Got response from server without an SSL error.
          if (this._finishCallback) {
            this._finishCallback();
          }
        }
      }).bind(this);
      req.addEventListener("load", xhrHandler);
      req.addEventListener("error", xhrHandler);
      req.send(null);
    } catch (e) {
      // We *expect* exceptions if there are problems with the certificate
      // presented by the site.  Log it, just in case, but we can proceed here,
      // with appropriate sanity checks
      Components.utils.reportError("Attempted to connect to a site with a bad certificate in the add exception dialog. " +
                                   "This results in a (mostly harmless) exception being thrown. " +
                                   "Logged for information purposes only: " + e);
    }
  },

  /**
   * Internal method to create an override.
   */
  _addOverride: function SSLE_addOverride() {
    let SSLStatus = this._sslStatus;
    let uri = this._uri;
    let flags = 0;

    if (SSLStatus.isUntrusted) {
      flags |= Ci.nsICertOverrideService.ERROR_UNTRUSTED;
    }
    if (SSLStatus.isDomainMismatch) {
      flags |= Ci.nsICertOverrideService.ERROR_MISMATCH;
    }
    if (SSLStatus.isNotValidAtThisTime) {
      flags |= Ci.nsICertOverrideService.ERROR_TIME;
    }

    CertOverrideService.rememberValidityOverride(
      uri.asciiHost,
      uri.port,
      SSLStatus.serverCert,
      flags,
      this._temporary);

    if (this._finishCallback) {
      this._finishCallback();
    }
  },

  /**
   * Creates a permanent exception to override all overridable errors for
   * the given URL.
   */
  addException: function SSLE_addException(aTemporary) {
    this._temporary = aTemporary;
    this._checkCert();
  }
};

let ErrorPage = {
  _addCertException: function(aMessage) {
    let frameLoaderOwner = aMessage.target.QueryInterface(Ci.nsIFrameLoaderOwner);
    let win = frameLoaderOwner.ownerDocument.defaultView;
    let mm = frameLoaderOwner.frameLoader.messageManager;

    let uri = Services.io.newURI(aMessage.data.url, null, null);
    let sslExceptions = new SSLExceptions((function() {
      mm.sendAsyncMessage('ErrorPage:ReloadPage');
    }).bind(this), uri, win);
    try {
      sslExceptions.addException(!aMessage.data.isPermanent);
    } catch (e) {
      dump("Failed to set cert exception: " + e + "\n");
    }
  },

  init: function errorPageInit() {
    Services.obs.addObserver(this, 'in-process-browser-or-app-frame-shown', false);
    Services.obs.addObserver(this, 'remote-browser-frame-shown', false);
  },

  observe: function errorPageObserve(aSubject, aTopic, aData) {
    let frameLoader = aSubject.QueryInterface(Ci.nsIFrameLoader);
    let mm = frameLoader.messageManager;

    // This won't happen from dom/ipc/preload.js in non-OOP builds.
    try {
      if (Services.prefs.getBoolPref("dom.ipc.tabs.disabled") === true) {
        mm.loadFrameScript(kErrorPageFrameScript, true, true);
      }
    } catch (e) {
      dump('Error loading ' + kErrorPageFrameScript + ' as frame script: ' + e + '\n');
    }
    mm.addMessageListener('ErrorPage:AddCertException', this._addCertException.bind(this));
  }
};

ErrorPage.init();
